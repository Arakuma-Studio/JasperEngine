// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A layered PSD into a Title Art document (docs/specs/S87-title-localisation-interchange.md R33,
// R44, AC17, AC24's profile note) over fixtures/psd/ (tools/titleart/make_psd_fixtures.py): groups,
// pass-through as Normal with a note, clipping, masks, a text layer's EngineData, the 13 adjustment
// records, fill layers, 16-bit, PSB, Grayscale, CMYK refused for layers but its styles read, an
// Adobe RGB profile noted; the create.from kinds and the apply_ps_style / import_ps_styles ops.

#include <gtest/gtest.h>

#include "texsynth/SynthImage.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/interchange/psd/PsdImport.h"
#include "titleart/interchange/psd/PsdReader.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
using titleart::Json;

namespace {

    std::string fixture(const char* name) {
        return std::string(JASPER_TEST_REPO_DIR) + "/editor/tests/titleart/fixtures/psd/" + name;
    }

    class PsdImportTest : public ::testing::Test {
    protected:
        fs::path project;
        void SetUp() override {
            project = fs::temp_directory_path() / ("je_psd_import_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                                                   ::testing::UnitTest::GetInstance()->current_test_info()->name());
            fs::remove_all(project);
            fs::create_directories(project / "assets" / "titles");
            psd::registerPsdInterchange();
        }
        void TearDown() override { fs::remove_all(project); }

        TitleCreateRequest request(const char* file) const {
            TitleCreateRequest r;
            r.sourcePath = fixture(file);
            r.documentPath = (project / "assets" / "titles" / "logo.titleart").string();
            r.projectPath = project.string();
            r.assetsPath = (project / "assets").string();
            return r;
        }

        bool import(const char* file, TitleCreateResult& out, std::string& code, std::string& error) const {
            return TitleArtOps::createFrom(request(file), out, code, error);
        }
    };

    const Json* layerNamed(const Json& list, const std::string& name) {
        for (const Json& l : list) {
            if (l.value("name", std::string()) == name) return &l;
            if (l.contains("children")) {
                if (const Json* found = layerNamed(l["children"], name)) return found;
            }
        }
        return nullptr;
    }

    bool noted(const TitleCreateResult& r, const std::string& text, const std::string& code = {}) {
        for (const Json& n : r.notes) {
            const std::string line = n.value("what", std::string());
            if (line.find(text) != std::string::npos && (code.empty() || n.value("code", std::string()) == code)) return true;
        }
        return false;
    }

    const TitleOpFile* fileEnding(const TitleCreateResult& r, const std::string& suffix) {
        for (const TitleOpFile& f : r.files) {
            if (f.path.size() >= suffix.size() && f.path.compare(f.path.size() - suffix.size(), suffix.size(), suffix) == 0) return &f;
        }
        return nullptr;
    }

}

TEST_F(PsdImportTest, LayeredRgb8) {
    TitleCreateResult r;
    std::string code, error;
    ASSERT_TRUE(import("layers8.psd", r, code, error)) << code << ": " << error;
    EXPECT_EQ(r.kind, "psd");
    const Json& root = r.doc.root;
    EXPECT_EQ(root["canvas"]["width"], 64);
    EXPECT_EQ(root["global_light"]["angle"].get<double>(), 120);
    const Json& layers = root["layers"];
    // groups: the pass-through group with its two children, drawn Normal with a note
    const Json* group = layerNamed(layers, "Group");
    ASSERT_NE(group, nullptr);
    EXPECT_EQ((*group)["type"], "group");
    EXPECT_EQ((*group)["blend"], "normal");
    ASSERT_EQ((*group)["children"].size(), 2u);
    EXPECT_EQ((*group)["children"][0]["name"], "Masked");
    EXPECT_TRUE(noted(r, "pass-through group drawn as Normal"));
    // masks and clipping
    const Json* masked = layerNamed(layers, "Masked");
    ASSERT_NE(masked, nullptr);
    EXPECT_EQ((*masked)["type"], "image");
    ASSERT_EQ((*masked)["masks"].size(), 1u);
    EXPECT_EQ((*masked)["masks"][0]["type"], "image");
    EXPECT_FALSE((*masked)["masks"][0]["linked"].get<bool>());
    EXPECT_EQ((*masked)["masks"][0]["channel"], "luminance");
    const Json* clipped = layerNamed(layers, "Clipped");
    ASSERT_NE(clipped, nullptr);
    EXPECT_TRUE((*clipped)["clip"].get<bool>());
    EXPECT_EQ((*clipped)["blend"], "screen");
    EXPECT_NEAR((*clipped)["opacity"].get<double>(), 128 / 255.0, 1e-6);
    EXPECT_DOUBLE_EQ((*clipped)["size"].get<double>(), 16.0) << "trimmed to its alpha: 16 x 16 of 32 x 16";
    // the text layer
    const Json* title = layerNamed(layers, "Title");
    ASSERT_NE(title, nullptr);
    EXPECT_EQ((*title)["type"], "text");
    EXPECT_EQ((*title)["text"], "BIG WIN");
    EXPECT_DOUBLE_EQ((*title)["size"].get<double>(), 48.0);
    EXPECT_DOUBLE_EQ((*title)["tracking"].get<double>(), 50.0);
    EXPECT_DOUBLE_EQ((*title)["line_height"].get<double>(), 2.5);
    EXPECT_EQ((*title)["align"], "center");
    EXPECT_EQ((*title)["style"]["face"]["fill"]["color"], "#ffcc00");
    EXPECT_EQ((*title)["style"]["rims"].size(), 1u) << "its layer style";
    EXPECT_TRUE(noted(r, "JasperFixtureSans-Bold", "font_unavailable"));
    EXPECT_TRUE(noted(r, "2 style runs"));
    // the 13 adjustments, the unsupported one dropped
    std::set<std::string> types;
    for (const Json& l : layers) {
        if (l["type"] == "adjustment") types.insert(l["adjustment"]["type"].get<std::string>());
    }
    EXPECT_EQ(types, (std::set<std::string>{"levels", "curves", "hue_saturation", "brightness_contrast", "exposure", "vibrance", "color_balance",
                                            "black_white", "photo_filter", "gradient_map", "invert", "posterize", "threshold"}));
    EXPECT_EQ(layerNamed(layers, "Adjust selc"), nullptr);
    EXPECT_TRUE(noted(r, "'selc' has no Title Art equivalent"));
    const Json* levels = layerNamed(layers, "Adjust levl");
    ASSERT_NE(levels, nullptr);
    EXPECT_DOUBLE_EQ((*levels)["adjustment"]["channels"]["rgb"]["in_black"].get<double>(), 10.0);
    EXPECT_DOUBLE_EQ((*levels)["adjustment"]["channels"]["rgb"]["gamma"].get<double>(), 1.2);
    const Json* hue = layerNamed(layers, "Adjust hue2");
    EXPECT_DOUBLE_EQ((*hue)["adjustment"]["hue"].get<double>(), 30.0);
    EXPECT_DOUBLE_EQ((*layerNamed(layers, "Adjust post"))["adjustment"]["levels"].get<double>(), 5.0);
    // fill layers
    const Json* solid = layerNamed(layers, "Solid Fill");
    ASSERT_NE(solid, nullptr);
    EXPECT_EQ((*solid)["type"], "generator");
    EXPECT_EQ((*solid)["generator"]["type"], "fill");
    EXPECT_EQ((*solid)["generator"]["params"]["color"], "#0ac81e");
    EXPECT_EQ((*layerNamed(layers, "Gradient Fill"))["generator"]["params"]["kind"], "radial");
    // blend, visibility, fill opacity and knockout
    const Json* hidden = layerNamed(layers, "Hidden Styled");
    ASSERT_NE(hidden, nullptr);
    EXPECT_FALSE((*hidden)["visible"].get<bool>());
    EXPECT_EQ((*hidden)["blend"], "multiply");
    EXPECT_NEAR((*hidden)["style"]["face"]["opacity"].get<double>(), 128 / 255.0, 1e-6);
    EXPECT_TRUE((*hidden)["style"]["knockout"].get<bool>());
    EXPECT_EQ((*hidden)["effects"][0]["type"], "drop_shadow");
    // a vector mask rasterised, a blend Title Art lacks drawn Normal
    const Json* vector = layerNamed(layers, "Vector Masked");
    ASSERT_NE(vector, nullptr);
    ASSERT_EQ((*vector)["masks"].size(), 1u);
    EXPECT_TRUE(noted(r, "vector mask rasterised"));
    EXPECT_EQ((*layerNamed(layers, "Dissolve"))["blend"], "normal");
    EXPECT_TRUE(noted(r, "blend mode 'diss'"));
    // the files and the report kept in the document
    ASSERT_NE(fileEnding(r, "logo_assets/Masked.png"), nullptr);
    ASSERT_NE(fileEnding(r, "logo_assets/Masked_mask.png"), nullptr);
    texsynth::Image mask;
    const TitleOpFile* maskFile = fileEnding(r, "logo_assets/Masked_mask.png");
    ASSERT_TRUE(texsynth::loadImageFromMemory(reinterpret_cast<const uint8_t*>(maskFile->content.data()), maskFile->content.size(), mask, error));
    EXPECT_EQ(mask.width, 64) << "the mask covers the canvas";
    EXPECT_EQ(mask.pixel(9, 9)[0], 255);
    EXPECT_EQ(mask.pixel(20, 9)[0], 0);
    EXPECT_EQ(mask.pixel(60, 60)[0], 0) << "the mask's default colour outside its rectangle";
    EXPECT_EQ(root["import_notes"].size(), r.notes.size());
    EXPECT_EQ((*masked)["image"]["path"], "titles/logo_assets/Masked.png");
    // the document is canonical
    titleart::TitleArtDoc again = r.doc;
    titleart::normalizeTitleArt(again, nullptr);
    EXPECT_EQ(titleart::writeTitleArt(again), titleart::writeTitleArt(r.doc));
}

TEST_F(PsdImportTest, SixteenBitPsbAndGrey) {
    TitleCreateResult r;
    std::string code, error;
    ASSERT_TRUE(import("layers16.psd", r, code, error)) << error;
    ASSERT_EQ(r.doc.root["layers"].size(), 2u);
    const TitleOpFile* top = fileEnding(r, "Top.png");
    ASSERT_NE(top, nullptr);
    texsynth::Image img;
    ASSERT_TRUE(texsynth::loadImageFromMemory(reinterpret_cast<const uint8_t*>(top->content.data()), top->content.size(), img, error));
    EXPECT_EQ(img.bitDepth, 16) << "a 16-bit layer stays 16-bit";
    uint16_t red;
    std::memcpy(&red, img.pixel(0, 0), 2);
    EXPECT_EQ(red, 250 * 257);
    ASSERT_TRUE(import("layers.psb", r, code, error)) << error;
    ASSERT_EQ(r.doc.root["layers"].size(), 1u);
    EXPECT_EQ(r.doc.root["layers"][0]["name"], "Only");
    ASSERT_TRUE(import("grey8.psd", r, code, error)) << error;
    const TitleOpFile* grey = fileEnding(r, "Grey.png");
    ASSERT_NE(grey, nullptr);
    ASSERT_TRUE(texsynth::loadImageFromMemory(reinterpret_cast<const uint8_t*>(grey->content.data()), grey->content.size(), img, error));
    EXPECT_EQ(img.pixel(3, 3)[0], 90);
    EXPECT_EQ(img.pixel(3, 3)[1], 90);
}

TEST_F(PsdImportTest, CmykRefusedButItsStylesRead) {
    TitleCreateResult r;
    std::string code, error;
    EXPECT_FALSE(import("cmyk.psd", r, code, error));
    EXPECT_EQ(code, "psd_mode_unsupported");
    EXPECT_NE(error.find("CMYK"), std::string::npos);
    std::ifstream in(fixture("cmyk.psd"), std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    psd::PsStyleFile styles;
    ASSERT_TRUE(psd::readPsStyles(bytes.data(), bytes.size(), styles, error)) << error;
    EXPECT_EQ(styles.styles.size(), 1u);
}

TEST_F(PsdImportTest, OtherProfilesAreNoted) {
    TitleCreateResult r;
    std::string code, error;
    ASSERT_TRUE(import("adobergb.psd", r, code, error)) << error;
    EXPECT_TRUE(noted(r, "Adobe RGB (1998)", "color_profile_ignored"));
}

TEST_F(PsdImportTest, StyleLibrariesAreNotDocuments) {
    TitleCreateResult r;
    std::string code, error;
    EXPECT_FALSE(import("styles.asl", r, code, error));
    EXPECT_EQ(code, "ps_styles_not_a_document");
    EXPECT_NE(error.find("import_ps_styles"), std::string::npos);
}

TEST_F(PsdImportTest, ApplyAndImportStyleOps) {
    titleart::TitleArtDoc doc = titleart::makeNewTitleArt();
    TitleOpsOptions options;
    options.documentPath = (project / "assets" / "titles" / "logo.titleart").string();
    options.projectPath = project.string();
    options.assetsPath = (project / "assets").string();
    TitleOpsResult applied = TitleArtOps::apply(doc, Json{{"op", "apply_ps_style"}, {"layer", "Title"}, {"file", fixture("styles.asl")},
                                                          {"style", "Shadows"}}, options);
    ASSERT_TRUE(applied.ok) << applied.error();
    const Json& title = applied.doc.root["layers"][0];
    ASSERT_EQ(title["effects"].size(), 3u);
    // px params scale with the layer's size (200) over the reference (100)
    const Json* drop = nullptr;
    for (const Json& e : title["effects"]) {
        if (e["type"] == "drop_shadow" && !drop) drop = &e;
    }
    ASSERT_NE(drop, nullptr);
    EXPECT_DOUBLE_EQ((*drop)["params"]["distance"].get<double>(), 4.0) << "2 px at 100 px is 4 px at 200 px";
    EXPECT_EQ(applied.undoName, "Apply Photoshop Style Shadows");
    EXPECT_TRUE(applied.reports.contains("0"));
    TitleOpsResult bitmap = TitleArtOps::apply(doc, Json{{"op", "apply_ps_style"}, {"layer", "Title"}, {"file", fixture("styles.asl")},
                                                         {"style", "Bitmap Pattern"}}, options);
    ASSERT_TRUE(bitmap.ok) << bitmap.error();
    ASSERT_EQ(bitmap.files.size(), 1u);
    EXPECT_NE(bitmap.files[0].path.find("logo_assets"), std::string::npos);
    EXPECT_NE(bitmap.files[0].path.find("patterns"), std::string::npos);
    EXPECT_EQ(bitmap.doc.root["layers"][0]["style"]["face"]["pattern"]["kind"], "custom");
    TitleOpsResult bad = TitleArtOps::apply(doc, Json{{"op", "apply_ps_style"}, {"layer", "Title"}, {"file", fixture("styles.asl")}, {"style", "Nope"}},
                                            options);
    ASSERT_FALSE(bad.ok);
    // import_ps_styles: one .titlelook per style in the project's library
    TitleOpsResult looks = TitleArtOps::apply(doc, Json{{"op", "import_ps_styles"}, {"file", fixture("styles.asl")}}, options);
    ASSERT_TRUE(looks.ok) << looks.error();
    size_t titlelooks = 0;
    for (const TitleOpFile& f : looks.files) {
        if (f.path.size() > 10 && f.path.compare(f.path.size() - 10, 10, ".titlelook") == 0) {
            titlelooks++;
            const Json look = Json::parse(f.content);
            EXPECT_EQ(look["format"], "jasperengine.titlelook");
            EXPECT_EQ(look["family"], "custom");
            EXPECT_EQ(look["tags"][0], "photoshop");
            EXPECT_EQ(look["tags"][1], "styles");
            EXPECT_NE(f.path.find("EditorSettings"), std::string::npos);
        }
    }
    EXPECT_EQ(titlelooks, 17u);
    EXPECT_EQ(looks.reports["0"]["looks"].size(), 17u);
    // the bitmap pattern travels in the look's .files folder
    bool patternFile = false;
    for (const TitleOpFile& f : looks.files) patternFile = patternFile || f.path.find("bitmap-pattern.files") != std::string::npos;
    EXPECT_TRUE(patternFile);
}

TEST_F(PsdImportTest, MalformedFilesFail) {
    std::ifstream in(fixture("layers8.psd"), std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (size_t cut : {size_t(10), size_t(40), size_t(300), bytes.size() / 2, bytes.size() - 7}) {
        psd::PsdFile file;
        std::string error;
        EXPECT_FALSE(psd::readPsd(bytes.data(), cut, file, error)) << cut;
        EXPECT_FALSE(error.empty());
    }
    // a corrupted layer length names the offset
    std::vector<uint8_t> mutated = bytes;
    mutated[60] ^= 0x7f;
    psd::PsdFile file;
    std::string error;
    if (!psd::readPsd(mutated.data(), mutated.size(), file, error)) EXPECT_NE(error.find("offset"), std::string::npos) << error;
}
