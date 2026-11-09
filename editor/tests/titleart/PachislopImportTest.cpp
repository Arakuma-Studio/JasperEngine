// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Pachislop project import (docs/specs/S87-title-localisation-interchange.md R28-R31, §6.8,
// AC14, AC15): each fixture of editor/tests/titleart/fixtures/pachislop/ (made by
// tools/titleart/make_pachislop_fixtures.py) imports to its committed `.titleart` golden
// (JASPER_TEST_UPDATE_GOLDEN=1 rewrites them) and its report lists exactly the items the fixture
// seeds; the v4 file and its v11 twin import identically; assets are extracted once (a second
// import writes nothing new); builtin fonts come from the font pack; the v4 per-character scale
// keeps every glyph where v4 drew it.

#include <gtest/gtest.h>

#include "titleart/TitleArtAssets.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/interchange/TitleImport.h"
#include "titleart/interchange/pachislop/PachislopImport.h"
#include "titleart/interchange/pachislop/PachislopMapper.h"
#include "titleart/text/TitleTextLayout.h"
#include "util/FontFace.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;
namespace fs = std::filesystem;

namespace {

    const fs::path FIXTURES = fs::path(JASPER_TEST_REPO_DIR) / "editor" / "tests" / "titleart" / "fixtures" / "pachislop";
    const fs::path FONTS = fs::path(JASPER_TEST_REPO_DIR) / "engine" / "tests" / "core" / "titleart" / "fonts";
    const char* FIXED_GUID = "00000000000000000000000000000087";

    std::string readText(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream s;
        s << in.rdbuf();
        return s.str();
    }

    class PachislopImportTest : public ::testing::Test {
    protected:
        static fs::path library;

        static void SetUpTestSuite() {
            library = fs::temp_directory_path() / "je_pachislop_library";
            fs::remove_all(library);
            // S84's font pack: the builtin dela-gothic-one (Pachislop's default font)
            const fs::path font = library / "fonts" / "dela-gothic-one";
            fs::create_directories(font);
            fs::copy_file(FONTS / "DelaGothicOne-Regular.ttf", font / "DelaGothicOne-Regular.ttf");
            fs::copy_file(FONTS / "DelaGothicOne-OFL.txt", font / "OFL.txt");
            // the pack's manifest, which S84's ImportPlan reads
            std::ofstream(library / "fonts" / "fonts.json")
                << R"({"version": 1, "fonts": {"dela-gothic-one": {"family": "Dela Gothic One", "style": "Regular", "files": ["DelaGothicOne-Regular.ttf"],)"
                   R"( "scripts": ["latin", "ja"], "category": "display"}}})";
#if defined(_WIN32)
            _putenv_s("JASPERENGINE_TITLEART_LIBRARY_DIR", library.string().c_str());
#else
            setenv("JASPERENGINE_TITLEART_LIBRARY_DIR", library.string().c_str(), 1);
#endif
            registerPachislopImport();
        }

        void SetUp() override {
            project = fs::temp_directory_path() / ("je_pachislop_import_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
            fs::remove_all(project);
            fs::create_directories(project / "assets" / "titles");
        }
        void TearDown() override { fs::remove_all(project); }

        fs::path project;

        TitleCreateRequest request(const fs::path& source, const std::string& name) const {
            TitleCreateRequest r;
            r.sourcePath = source.string();
            r.documentPath = (project / "assets" / "titles" / (name + ".titleart")).string();
            r.projectPath = project.string();
            r.assetsPath = (project / "assets").string();
            return r;
        }

        TitleCreateResult import(const fs::path& source, const std::string& name) const {
            TitleCreateResult out;
            std::string code, error;
            EXPECT_TRUE(TitleArtOps::createFrom(request(source, name), out, code, error)) << code << ": " << error;
            EXPECT_EQ(out.kind, "pachislop");
            out.doc.root["guid"] = FIXED_GUID;
            return out;
        }

        // what a commit writes: the import's files on disk
        static void writeFiles(const TitleCreateResult& r) {
            for (const TitleOpFile& f : r.files) {
                fs::create_directories(fs::path(f.path).parent_path());
                std::ofstream(fs::path(f.path), std::ios::binary) << f.content;
            }
        }
    };

    fs::path PachislopImportTest::library;

    // the report's (code or field) of each note, sorted
    std::vector<std::string> noteKeys(const TitleCreateResult& r) {
        std::vector<std::string> keys;
        for (const Json& n : r.notes) {
            const std::string code = n.value("code", std::string());
            keys.push_back(code.empty() ? n.value("action", std::string()) + ":" + n.value("field", std::string()) : code);
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    }

    // the seeded items of each fixture (make_pachislop_fixtures.py's docstrings)
    struct Fixture {
        const char* name;
        std::vector<std::string> notes;
    };

    const std::vector<Fixture>& fixtures() {
        static const std::vector<Fixture> list = {
            {"v4_text_rims", {"approximated:style.face.reflection.map", "dropped:solo", "note:"}},
            {"v4_text_rims_v11", {"approximated:style.face.reflection.map", "dropped:solo"}},
            {"v6_groups_masks_clip", {"mask_layer_missing", "note:"}},
            // the Glints companion the content-effects step makes (companionOf: S83 D3's note)
            {"v7_content_effects", {"dropped:generator", "note:", "unknown_type"}},
            {"v8_lighting", {"approximated:comp.camera", "note:"}},
            {"v10_rings", {"approximated:comp.camera", "note:"}},
            {"v11_all_kinds", {"approximated:source.assetId", "font_unavailable", "image_format_unsupported"}},
        };
        return list;
    }

}

TEST_F(PachislopImportTest, MatchesGolden) {
    const bool update = std::getenv("JASPER_TEST_UPDATE_GOLDEN") != nullptr;
    for (const Fixture& f : fixtures()) {
        SCOPED_TRACE(f.name);
        const TitleCreateResult r = import(FIXTURES / (std::string(f.name) + ".pslot.json"), f.name);
        const std::string text = titleart::writeTitleArt(r.doc);
        const fs::path golden = FIXTURES / (std::string(f.name) + ".titleart");
        if (update) {
            std::ofstream(golden, std::ios::binary) << text;
        } else {
            EXPECT_EQ(text, readText(golden)) << "the import of " << f.name << " changed: JASPER_TEST_UPDATE_GOLDEN=1 rewrites the golden";
        }
        EXPECT_EQ(noteKeys(r), f.notes) << r.notes.dump(1);
        // the document keeps the report's lines
        EXPECT_EQ(r.doc.root["import_notes"].size(), r.notes.size());
        // the golden parses back to itself (the import is a normalized document)
        const titleart::TitleArtParseResult parsed = titleart::parseTitleArt(text);
        ASSERT_TRUE(parsed.ok) << parsed.error.toString();
        EXPECT_EQ(titleart::writeTitleArt(parsed.doc), text);
    }
}

TEST_F(PachislopImportTest, TheV4FileAndItsV11TwinImportIdentically) {
    TitleCreateResult v4 = import(FIXTURES / "v4_text_rims.pslot.json", "same");
    TitleCreateResult v11 = import(FIXTURES / "v4_text_rims_v11.pslot.json", "same");
    // the upgrade note is the only difference
    v4.doc.root.erase("import_notes");
    v11.doc.root.erase("import_notes");
    EXPECT_EQ(titleart::writeTitleArt(v4.doc), titleart::writeTitleArt(v11.doc));
}

TEST_F(PachislopImportTest, AssetsAreExtractedAndReimportWritesNothingNew) {
    const TitleCreateResult first = import(FIXTURES / "v11_all_kinds.pslot.json", "kinds");
    std::set<std::string> names;
    for (const TitleOpFile& f : first.files) names.insert(fs::path(f.path).lexically_relative(project / "assets").generic_string());
    const std::set<std::string> expected = {"titles/kinds_assets/Oswald.ttf", "titles/kinds_assets/badge.svg", "titles/kinds_assets/grad.png",
                                            "titles/kinds_assets/flag.png"};
    EXPECT_EQ(names, expected) << "fonts and images extracted, the GIF as a PNG, the WebP left out";
    for (const TitleOpFile& f : first.files) {
        if (fs::path(f.path).filename() == "flag.png") {
            EXPECT_EQ(f.content.substr(1, 3), "PNG");
        }
    }
    writeFiles(first);
    const TitleCreateResult again = import(FIXTURES / "v11_all_kinds.pslot.json", "kinds");
    EXPECT_TRUE(again.files.empty()) << "every file is reused by its SHA-256";
    EXPECT_EQ(titleart::writeTitleArt(again.doc), titleart::writeTitleArt(first.doc));
}

TEST_F(PachislopImportTest, BuiltinFontsComeFromTheFontPackOncePerProject) {
    const TitleCreateResult first = import(FIXTURES / "v4_text_rims.pslot.json", "pack");
    bool font = false, licence = false;
    for (const TitleOpFile& f : first.files) {
        const std::string relative = fs::path(f.path).lexically_relative(project / "assets").generic_string();
        font = font || relative == "TitleArt/Fonts/dela-gothic-one/DelaGothicOne-Regular.ttf";
        licence = licence || relative == "TitleArt/Fonts/dela-gothic-one/OFL.txt";
    }
    EXPECT_TRUE(font);
    EXPECT_TRUE(licence) << "the pack font's OFL text comes with it";
    EXPECT_EQ(first.doc.root["layers"][0]["font"]["path"], "TitleArt/Fonts/dela-gothic-one/DelaGothicOne-Regular.ttf");
    writeFiles(first);
    const TitleCreateResult second = import(FIXTURES / "v4_text_rims.pslot.json", "pack2");
    for (const TitleOpFile& f : second.files) EXPECT_EQ(fs::path(f.path).string().find("TitleArt"), std::string::npos) << f.path;
}

TEST_F(PachislopImportTest, AV4CharacterScaleKeepsEveryGlyphWhereV4DrewIt) {
    // v4: "ABC" with B scaled 1.5 and offset; v5 reflows the line, the import moves it back
    const Json chars = Json{{"1", Json{{"dx", 2}, {"dy", 1}, {"scale", 1.5}, {"rotate", 0}}}};
    const Json layer = Json{{"id", "0000aa01"}, {"name", "T"}, {"kind", "title2d"}, {"visible", true}, {"blend", "normal"},
                            {"transform", Json{{"x", 30}, {"y", -10}, {"rotZ", 0}, {"scale", 1}, {"opacity", 1}}}, {"effects", Json::array()},
                            {"source", Json{{"kind", "text"}, {"text", "ABC"}, {"fontId", "dela-gothic-one"}, {"size", 200}, {"tracking", 0},
                                            {"lineHeight", 1.1}, {"align", "center"}, {"vertical", false}, {"shear", 0}, {"chars", chars}}},
                            {"style", Json{{"face", Json{{"fill", Json{{"kind", "solid"}, {"color", "#ffffff"}}}}}, {"rims", Json::array()}}}};
    const Json file = Json{{"format", "pslot"}, {"version", 4},
                           {"project", Json{{"version", 4}, {"name", "Scale"}, {"comp", Json{{"width", 1280}, {"height", 720}}}, {"layers", Json::array({layer})}}},
                           {"fonts", Json::array()}, {"assets", Json::array()}};
    const fs::path source = project / "scale.pslot.json";
    std::ofstream(source) << file.dump();
    const TitleCreateResult r = import(source, "scale");
    writeFiles(r);
    bool noted = false;
    for (const Json& n : r.notes) noted = noted || n.value("what", std::string()).find("Per-character scale") != std::string::npos;
    EXPECT_TRUE(noted);

    class Resolver : public titleart::AssetResolver {
    public:
        explicit Resolver(fs::path a) : assets(std::move(a)) {}
        titleart::ResolvedTitleAsset resolve(const titleart::TitleAssetRef& ref, titleart::TitleAssetKind) const override {
            titleart::ResolvedTitleAsset out;
            out.found = fs::exists(assets / ref.path);
            out.path = (assets / ref.path).string();
            out.assetPath = ref.path;
            return out;
        }
        std::shared_ptr<FontFace> loadFont(const titleart::ResolvedTitleAsset& a, int index, std::string* error) const override {
            return FontFace::loadFile(a.path, index, error, FontFaceRasterizer::Optional);
        }
        bool readBytes(const titleart::ResolvedTitleAsset&, std::vector<uint8_t>&, std::string*) const override { return false; }
        bool loadImage(const titleart::ResolvedTitleAsset&, titleart::TitleImageData&, std::string*) const override { return false; }
        std::shared_ptr<FontFace> builtinFont() const override { return nullptr; }
        fs::path assets;
    } resolver(project / "assets");

    const Json& imported = r.doc.root["layers"][0];
    // v4's positions: every scale 1 (v4 laid the line out unscaled), the stored offsets, the stored position
    Json v4 = imported;
    v4["chars"] = Json{{"1", Json{{"offset", Json::array({2, 1})}, {"scale", 1}}}};
    v4["transform"]["position"] = Json::array({30, -10});
    titleart::TitleTextLayout layout(&resolver);
    std::map<uint32_t, std::pair<double, double>> want;
    for (const auto& c : layout.layout(v4).clusters) want[c.index] = {30 + c.emCell.x + c.emCell.width / 2, -10 + c.emCell.y + c.emCell.height / 2};
    const titleart::TitleVec2 p = titleart::decodeVec2(imported["transform"]["position"]);
    for (const auto& c : layout.layout(imported).clusters) {
        SCOPED_TRACE(c.index);
        EXPECT_NEAR(p.x + c.emCell.x + c.emCell.width / 2, want[c.index].first, 0.01);
        EXPECT_NEAR(p.y + c.emCell.y + c.emCell.height / 2, want[c.index].second, 0.01);
    }
}

TEST_F(PachislopImportTest, ReimportPachislop3DNeedsAKeptStyle) {
    titleart::TitleArtDoc doc = titleart::makeNewTitleArt();
    const TitleOpsResult none = TitleArtOps::apply(doc, Json{{"op", "reimport_pachislop_3d"}, {"layer", "Title"}});
    ASSERT_FALSE(none.ok);
    EXPECT_EQ(none.problems.front().code, pachislop::mapperHas3D() ? "bad_layer" : "not_available");
    if (!pachislop::mapperHas3D()) return;
    doc.root["layers"][0]["pachislop_style3d"] = Json{{"depth", 12}, {"face", Json{{"preset", "gold"}, {"color", "#ffcc33"}}}, {"bands", Json::array()}};
    titleart::normalizeTitleArt(doc, nullptr);
    const TitleOpsResult r = TitleArtOps::apply(doc, Json{{"op", "reimport_pachislop_3d"}});
    ASSERT_TRUE(r.ok) << r.error();
    const Json& layer = r.doc.root["layers"][0];
    EXPECT_FALSE(layer.contains("pachislop_style3d"));
    EXPECT_EQ(layer["dimension"], "3d");
    EXPECT_TRUE(layer["style_3d"].is_object());
    EXPECT_EQ(r.undoName, "Reimport Pachislop 3D");
}

TEST_F(PachislopImportTest, RefusesOldNewerAndOtherFiles) {
    const auto attempt = [&](const std::string& text) {
        const fs::path source = project / "x.pslot.json";
        std::ofstream(source) << text;
        TitleCreateResult out;
        std::string code, error;
        EXPECT_FALSE(TitleArtOps::createFrom(request(source, "x"), out, code, error));
        return code + ": " + error;
    };
    const Json comp = Json{{"width", 100}, {"height", 100}};
    EXPECT_NE(attempt(Json{{"format", "pslot"}, {"version", 3}, {"project", Json{{"version", 3}, {"comp", comp}, {"layers", Json::array()}}}}.dump())
                  .find("This is a version 3 project; this version of the app opens version 4 to 11 projects only"),
              std::string::npos);
    EXPECT_EQ(attempt(Json{{"format", "pslot"}, {"version", 12}, {"project", Json{{"version", 12}, {"comp", comp}, {"layers", Json::array()}}}}.dump())
                  .rfind("pachislop_version_newer", 0),
              0u);
    EXPECT_EQ(attempt("{\"format\": \"pslot\", broken").rfind("not_json", 0), 0u);
}
