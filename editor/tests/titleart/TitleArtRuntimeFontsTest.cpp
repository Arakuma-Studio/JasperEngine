// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The fonts an export ships for its Realtime titles (docs/specs/S81-title-art-runtime.md R54, R56,
// AC15 export part): the font a runtime document reads ships as a subset of the characters the
// document, its instances' Additional Characters and their text overrides need; the build's copy
// of the document names the subset; the credits list it. Baked-only titles ship no font.

#include <gtest/gtest.h>

#include "AppSettings.h"
#include "Project.h"
#include "support/EditorTestSupport.h"
#include "titleart/TitleArtIO.h"
#include "titleart/runtime/TitleArtRuntimeFonts.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <unistd.h>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    constexpr const char* FONT = "assets/fonts/SourceSans3-Regular.otf";
    constexpr const char* DOC = "assets/titles/score.titleart";

    struct Headless {
        bool ready = false;
        std::unique_ptr<Project> project;
        fs::path dir;
    };

    std::string readAll(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            jasperengine::editor::test::ensureEngine();
            const fs::path root = fs::temp_directory_path() / ("jasper-titleart-fonts-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets" / "titles");
            fs::create_directories(out.dir / "assets" / "fonts");
            std::ofstream(out.dir / "project.yaml") << "name: titleartfonts\n";
            fs::copy_file(fs::path(JASPER_TEST_REPO_DIR) / "engine" / "tests" / "core" / "titleart" / "fonts" / "SourceSans3-Regular.otf",
                          out.dir / FONT);
            // a text layer in the font, its Characters only the document's own text
            titleart::TitleArtDoc doc = titleart::makeNewTitleArt({800, 400, "WIN", ""});
            doc.layers()[0]["font"] = titleart::Json{{"path", FONT}, {"guid", ""}};
            doc.root["runtime"] = titleart::Json{{"characters", {{"presets", titleart::Json::array()}, {"document_text", true}}}};
            titleart::normalizeTitleArt(doc);
            std::ofstream(out.dir / DOC) << titleart::writeTitleArt(doc);
            AppSettings::initialize();
            out.project.reset(new Project());
            jasperengine::editor::test::useProjectPaths(out.project.get());
            out.ready = out.project->loadProject(out.dir, false);
            jasperengine::editor::test::releaseAtExit(out.project.get());
            return out;
        }();
        // the test System drops the project's paths at the end of every test
        if (h.project) jasperengine::editor::test::useProjectPaths(h.project.get());
        return h;
    }

    // the build's assets: the export copies every asset before the Title Art step
    fs::path buildAssets() {
        const fs::path build = headless().dir.parent_path() / "build-assets";
        fs::remove_all(build);
        fs::create_directories(build / "assets" / "titles");
        fs::create_directories(build / "assets" / "fonts");
        fs::copy_file(headless().dir / DOC, build / DOC);
        fs::copy_file(headless().dir / FONT, build / FONT);
        return build;
    }

    TitleArtExportPlan realtimePlan() {
        TitleArtExportPlan plan;
        plan.runtimeTitles.insert(DOC);
        TitleArtShippedInstance instance;
        instance.where = "scene Main";
        instance.object = "Score";
        instance.title = DOC;
        instance.mode = TitleArtRenderMode::REALTIME;
        instance.additionalCharacters = "0123456789";
        TitleArtOverride text;
        text.name = "text";
        text.type = "text";
        text.text = "JACKPOT";
        instance.overrides.push_back(text);
        plan.instances.push_back(instance);
        return plan;
    }

}

TEST(TitleArtRuntimeFonts, ARealtimeTitlesFontShipsAsTheSubsetItsDocumentNames) {
    ASSERT_TRUE(headless().ready);
    const fs::path build = buildAssets();
    const TitleArtFontShipResult result = shipTitleArtFonts(headless().project.get(), realtimePlan(), build);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.shipped.size(), 1u);
    ASSERT_EQ(result.credits.size(), 1u);
    EXPECT_TRUE(result.credits[0].subset);
    EXPECT_TRUE(result.credits[0].ofl) << "SourceSans3 is OFL";
    EXPECT_LT(result.bytesAfter * 4, result.bytesBefore) << "a few characters of a full Latin font: " << result.shipped[0];

    // the build's document names the subset, which is there and keeps the characters asked for
    const titleart::TitleArtParseResult shipped = titleart::parseTitleArt(readAll(build / DOC));
    ASSERT_TRUE(shipped.ok);
    const std::string fontPath = shipped.doc.layers()[0]["font"]["path"].get<std::string>();
    EXPECT_NE(fontPath, FONT);
    EXPECT_EQ(fontPath.rfind("assets/fonts/SourceSans3-Regular.subset-", 0), 0u) << fontPath;
    EXPECT_EQ(fs::path(fontPath).extension(), ".otf");
    ASSERT_TRUE(fs::is_regular_file(build / fontPath)) << fontPath;
    EXPECT_EQ(result.credits[0].file, fontPath);
    EXPECT_NE(result.shipped[0].find("(20 characters"), std::string::npos)
        << "W, I, N (the document), J, A, C, K, P, O, T (the override), 0-9 (Additional Characters): " << result.shipped[0];
    // the project's own document is not touched
    EXPECT_NE(readAll(headless().dir / DOC).find(FONT), std::string::npos);
}

TEST(TitleArtRuntimeFonts, BakedOnlyTitlesShipNoFont) {
    ASSERT_TRUE(headless().ready);
    const fs::path build = buildAssets();
    TitleArtExportPlan plan;
    plan.bakedTitles.insert(DOC);
    const TitleArtFontShipResult result = shipTitleArtFonts(headless().project.get(), plan, build);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.shipped.empty());
    EXPECT_TRUE(result.credits.empty());
    EXPECT_NE(readAll(build / DOC).find(FONT), std::string::npos) << "the document is left as it is";
}
