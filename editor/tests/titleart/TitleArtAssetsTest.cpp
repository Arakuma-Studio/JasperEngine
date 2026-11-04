// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// S80 R4, R5, AC5, AC6 on a headless project: a .titleart's dependency edges (fonts and images
// as title_art_asset with their JSON path, the baked PNGs as title_art_output), the relink of a
// moved font's path hint, and a moved document carrying its derived PNG, meta and sidecar with
// the recipe naming the new path; plus the document checks of the registry.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "JasperEngine.h"
#include "Project.h"
#include "asset/AssetIndex.h"
#include "asset/AssetOps.h"
#include "texsynth/SynthImage.h"
#include "titleart/TitleArtBake.h"
#include "titleart/TitleArtBuiltinChecks.h"
#include "titleart/TitleArtChecks.h"
#include "titleart/TitleArtDocument.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtRenderHost.h"
#include "util/AssetDatabase.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
using TJson = titleart::Json;

namespace {

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-titleart-assets-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets");
            std::ofstream(out.dir / "project.yaml") << "name: titleartassets\nassetsDir: assets\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            editor::test::useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            return out;
        }();
        if (h.project) editor::test::useProjectPaths(h.project.get());
        return h;
    }

    Project* project() { return headless().project.get(); }
    const fs::path& root() { return headless().dir; }

    std::string readAll(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    void writeAll(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << content;
    }

    void refresh() {
        project()->refreshAssetDatabase(true);
        project()->getAssetIndex().waitUntilReady();
    }

    // a document at assets/<dir>/<name> naming the font at assets/<font>
    titleart::TitleArtDoc documentWithFont(const std::string& fontPath, const std::string& fontGuid) {
        titleart::TitleArtDoc doc = titleart::makeNewTitleArt();
        doc.root["layers"][0]["font"] = TJson{{"path", fontPath}, {"guid", fontGuid}};
        doc.root["layers"][0]["fallback_fonts"] = TJson::array({TJson{{"path", "fonts/missing.otf"}, {"guid", ""}}});
        titleart::normalizeTitleArt(doc, nullptr);
        return doc;
    }

    class TitleArtAssetsTest : public ::testing::Test {
    protected:
        void SetUp() override { ASSERT_TRUE(headless().ready) << "the headless project did not open"; }
        void TearDown() override { TitleArtDocument::closeAll(); }
    };

}

TEST_F(TitleArtAssetsTest, TitleArtEdges) {
    writeAll(root() / "assets/edges/fonts/face.ttf", "not really a font");
    refresh();
    const std::string fontGuid = project()->getAssetDatabase().getGuid(fs::path("assets/edges/fonts/face.ttf"));
    ASSERT_FALSE(fontGuid.empty());
    const titleart::TitleArtDoc doc = documentWithFont("edges/fonts/face.ttf", fontGuid);
    writeAll(root() / "assets/edges/logo.titleart", titleart::writeTitleArt(doc));
    refresh();
    const Guid referrer = project()->getAssetDatabase().getAssetGuid(fs::path("assets/edges/logo.titleart"));
    ASSERT_FALSE(referrer.isNull());
    const std::vector<DepEdge> edges = AssetIndex::extractFile(project()->getAssetDatabase(), root(), {"assets", ""},
                                                              "assets/edges/logo.titleart", AssetType::TitleArt, referrer);
    bool font = false, fallback = false, output = false;
    for (const DepEdge& edge : edges) {
        if (edge.kind == DepEdgeKind::TitleArtAsset && edge.field == "layers[0].font") {
            font = true;
            EXPECT_EQ(edge.target.str(), fontGuid);
            EXPECT_EQ(edge.expected, AssetType::Font);
        }
        if (edge.kind == DepEdgeKind::TitleArtAsset && edge.field == "layers[0].fallback_fonts[0]") {
            fallback = true;
            EXPECT_EQ(edge.targetPath, "assets/fonts/missing.otf");
        }
        if (edge.kind == DepEdgeKind::TitleArtOutput) {
            output = true;
            EXPECT_EQ(edge.field, "outputs[0]");
            EXPECT_EQ(edge.targetPath, "assets/edges/logo.titleart.png");
        }
    }
    EXPECT_TRUE(font && fallback && output);
    EXPECT_STREQ(depEdgeKindName(DepEdgeKind::TitleArtOutput), "title_art_output");
    EXPECT_EQ(depEdgeKindFromName("title_art_asset"), DepEdgeKind::TitleArtAsset);
}

TEST_F(TitleArtAssetsTest, RelinkAndMove) {
    writeAll(root() / "assets/move/fonts/face.ttf", "font bytes");
    refresh();
    const std::string fontGuid = project()->getAssetDatabase().getGuid(fs::path("assets/move/fonts/face.ttf"));
    const titleart::TitleArtDoc doc = documentWithFont("move/fonts/face.ttf", fontGuid);
    const fs::path document = root() / "assets/move/logo.titleart";
    writeAll(document, titleart::writeTitleArt(doc));
    // a bake's PNG, its meta and the sidecar
    texsynth::Image image = texsynth::Image::make(4, 4, 4, 8);
    std::vector<uint8_t> png, withRecipe;
    ASSERT_TRUE(texsynth::encodePng(image, png));
    TitleBakeRecipe recipe;
    recipe.sourceGuid = doc.guid();
    recipe.sourcePath = "move/logo.titleart";
    recipe.sourceHash = "h";
    recipe.output = "main";
    std::string error;
    ASSERT_TRUE(insertTitleBakeRecipe(png, recipe, withRecipe, error)) << error;
    writeAll(root() / "assets/move/logo.titleart.png", std::string(withRecipe.begin(), withRecipe.end()));
    writeAll(titleArtSidecarPath(document), TitleBakeInfo().toText());
    refresh();
    const std::string pngGuid = project()->getAssetDatabase().getGuid(fs::path("assets/move/logo.titleart.png"));
    ASSERT_FALSE(pngGuid.empty());

    // the font moves: the document's hint follows, its GUID unchanged
    ASSERT_TRUE(AssetOps::move(*project(), root() / "assets/move/fonts/face.ttf", root() / "assets/move/fonts/renamed.ttf").ok);
    titleart::TitleArtParseResult parsed = titleart::parseTitleArt(readAll(document));
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.doc.root["layers"][0]["font"]["path"], "move/fonts/renamed.ttf");
    EXPECT_EQ(parsed.doc.root["layers"][0]["font"]["guid"], fontGuid);

    // the document moves with its PNG, meta and sidecar; the recipe names the new path
    const fs::path moved = root() / "assets/moved/title.titleart";
    fs::create_directories(moved.parent_path());
    ASSERT_TRUE(AssetOps::move(*project(), document, moved).ok);
    EXPECT_TRUE(fs::exists(root() / "assets/moved/title.titleart.png"));
    EXPECT_TRUE(fs::exists(root() / "assets/moved/title.titleart.png.meta"));
    EXPECT_TRUE(fs::exists(root() / "assets/moved/title.titleart.bake.json"));
    EXPECT_FALSE(fs::exists(root() / "assets/move/logo.titleart.png"));
    const std::optional<TitleBakeRecipe> after = readTitleBakeRecipe(root() / "assets/moved/title.titleart.png", &error);
    ASSERT_TRUE(after) << error;
    EXPECT_EQ(after->sourcePath, "moved/title.titleart");
    EXPECT_EQ(project()->getAssetDatabase().getGuid(fs::path("assets/moved/title.titleart.png")), pngGuid) << "references by GUID keep working";
}

TEST_F(TitleArtAssetsTest, DocumentChecks) {
    registerBuiltinTitleChecks();
    titleart::TitleArtDoc doc = titleart::makeNewTitleArt();
    doc.root["layers"][0]["text"] = "A\xe3\x81\x82";     // "A" and a kana the built-in face lacks
    doc.root["exposed"][0]["targets"][0]["layer"] = "deadbeef";
    titleart::normalizeTitleArt(doc, nullptr);
    TitleCheckContext context(doc, "assets/checks.titleart");
    const EditorTitleAssetResolver* assets = TitleArtRenderHost::editor().resolver(project());
    context.assets = assets;
    std::vector<const CheckDef*> checks;
    std::vector<std::string> valid;
    std::string error;
    ASSERT_TRUE(TitleArtChecks::select({"exposed_targets", "glyphs_present", "not_clipped"}, checks, error, valid)) << error;
    const TitleCheckReport report = TitleArtChecks::run(checks, context);
    ASSERT_EQ(report.checks.size(), 3u);
    EXPECT_EQ(report.checks[0].status, TitleCheckStatus::Fail);
    ASSERT_TRUE(report.checks[0].fix.contains("ops"));
    EXPECT_EQ(report.checks[0].fix["ops"][0]["op"], "unexpose");
    EXPECT_EQ(report.checks[1].status, TitleCheckStatus::Fail) << report.checks[1].message;
    EXPECT_EQ(report.checks[1].measured["missing"], 1);
    EXPECT_EQ(report.checks[2].status, TitleCheckStatus::Skip) << "a pixel check without a renderer is not measured";
    EXPECT_EQ(report.verdict, TitleCheckStatus::Fail);
    // the ids S80 registers are all there, once
    for (const std::string& id : builtinTitleCheckIds()) EXPECT_TRUE(TitleArtChecks::find(id)) << id;
}
