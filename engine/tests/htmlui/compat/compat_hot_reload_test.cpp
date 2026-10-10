// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Phase-2 integration (p2-integrate): hot reload of the files an .html document draws with,
// through UIDocumentSystem::reloadChangedFiles as the editor's watcher calls it. The document
// lives under the assets directory, as in a project: the compat layer hands RmlUi its
// references as asset:// URLs, and phase 1's reload names the changed file by its asset path.
// - an image replaced on disk (an <img> and a CSS background) draws with the new pixels, the
//   document and its elements stay;
// - a font replaced on disk (@font-face url()) reloads the document with the new face;
// - a linked sheet and an @imported sheet restyle in place; the document itself reloads.

#include "TestGoldenCommon.h"
#include "TestDocumentCommon.h"

#include "HtmlUI.h"
#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/ImportManifest.h"
#include "subsystem/UIDocumentSystem.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::test;
namespace hc = jasperengine::htmlui::compat;
namespace fs = std::filesystem;

namespace {

    // an uncompressed 24-bit BMP of one colour (the engine's image loader reads it)
    std::string bmp(int width, int height, uint8_t r, uint8_t g, uint8_t b){
        const int row = (width * 3 + 3) & ~3;
        const uint32_t size = 54 + (uint32_t)(row * height);
        std::string out(size, '\0');
        const auto put32 = [&out](size_t at, uint32_t value){
            for (int i = 0; i < 4; i++)
                out[at + (size_t)i] = (char)((value >> (8 * i)) & 0xFF);
        };
        out[0] = 'B'; out[1] = 'M';
        put32(2, size); put32(10, 54); put32(14, 40); put32(18, (uint32_t)width); put32(22, (uint32_t)height);
        out[26] = 1; out[28] = 24;
        put32(34, (uint32_t)(row * height));
        for (int y = 0; y < height; y++){
            for (int x = 0; x < width; x++){
                const size_t at = 54 + (size_t)(y * row + x * 3);
                out[at] = (char)b; out[at + 1] = (char)g; out[at + 2] = (char)r;
            }
        }
        return out;
    }

    const char* documentHtml =
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "  <link rel=\"stylesheet\" href=\"hot.css\">\n"
        "</head>\n"
        "<body>\n"
        "  <img id=\"pic\" src=\"pic.bmp\" alt=\"\">\n"
        "  <div id=\"layer\"></div>\n"
        "  <div id=\"imp\"></div>\n"
        "  <span id=\"txt\">Hot reload text</span>\n"
        "</body>\n"
        "</html>\n";

    const char* documentCss =
        "@import url(\"more.css\");\n"
        "@font-face { font-family: \"Hot\"; src: url(\"hot.ttf\") format(\"truetype\"); }\n"
        "* { margin: 0; padding: 0; }\n"
        "body { background: rgb(0, 0, 0); }\n"
        "#pic { position: absolute; left: 20px; top: 20px; width: 100px; height: 100px; }\n"
        "#layer { position: absolute; left: 140px; top: 20px; width: 100px; height: 100px; background: url(pic.bmp) center / cover; }\n"
        "#txt { position: absolute; left: 20px; top: 250px; font-family: \"Hot\", sans-serif; font-size: 32px; color: white; }\n";

    const char* importedCss = "#imp { position: absolute; left: 260px; top: 20px; width: 100px; height: 100px; background: rgb(0, 0, 255); }\n";

    class CompatHotReloadTest : public FullPathGoldenTest {
    protected:
        fs::path dir;

        void SetUp() override{
            FullPathGoldenTest::SetUp();
            ASSERT_EQ(HtmlUI::getReferenceCount(), 0) << "an earlier test left RmlUi initialised";
            const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
            dir = fs::path(JASPER_HTMLUI_TEST_BINARY_DIR) / "compat-hot-reload" / info->name();
            fs::remove_all(dir);
            fs::create_directories(dir / "ui");
            write("ui/hot.html", documentHtml);
            write("ui/hot.css", documentCss);
            write("ui/more.css", importedCss);
            write("ui/pic.bmp", bmp(8, 8, 255, 0, 255));
            fs::copy_file(fs::path(JASPER_HTMLUI_TEST_SOURCE_DIR) / "fixtures/compat/pages/htmlui-imports/fonts/inter/Inter-400.ttf",
                          dir / "ui/hot.ttf");
            // the directory is the project's assets directory
            TestSystem::get().setAssetPath(dir.generic_string());
            hc::ImportManifest::reload();
            hc::DocumentLoader::clearCache();
        }

        void TearDown() override{
            hc::DocumentLoader::clearCache();
            TestSystem::get().setAssetPath("");
            hc::ImportManifest::reload();
            EXPECT_EQ(HtmlUI::getReferenceCount(), 0);
            FullPathGoldenTest::TearDown();
        }

        // a new content and a later modification time, whatever the file system's resolution
        void write(const std::string& name, const std::string& content){
            const fs::path file = dir / name;
            std::error_code error;
            const bool existed = fs::exists(file, error);
            const fs::file_time_type before = existed ? fs::last_write_time(file, error) : fs::file_time_type();
            {
                std::ofstream out(file, std::ios::binary | std::ios::trunc);
                out << content;
            }
            if (existed)
                fs::last_write_time(file, before + std::chrono::seconds(2), error);
        }

        void replace(const std::string& name, const fs::path& source){
            std::ifstream in(source, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            ASSERT_FALSE(content.empty()) << source;
            write(name, content);
        }

        float widthOf(Rml::ElementDocument* document, const char* id){
            Rml::Element* element = document ? document->GetElementById(id) : nullptr;
            return element ? element->GetBox().GetSize(Rml::BoxArea::Border).x : -1.0f;
        }
    };

}

TEST_F(CompatHotReloadTest, ReplacedImageDrawsWithoutReloadingTheDocument){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument("ui/hot.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    UIDocumentSystem& system = documentSystem(scene);
    Rml::ElementDocument* rml = system.getDocument(document.getEntity());
    ASSERT_NE(rml, nullptr);
    ASSERT_NE(system.getCompiledDocument(document.getEntity()), nullptr);

    Screen frame;
    ASSERT_TRUE(capture(frame, 3));
    ASSERT_TRUE(frame.is(70, 70, 255, 0, 255)) << "the <img>";
    ASSERT_TRUE(frame.is(190, 70, 255, 0, 255)) << "the CSS background";
    const std::vector<std::string> files = system.getDocumentFiles(document.getEntity());
    EXPECT_NE(std::find(files.begin(), files.end(), "ui/pic.bmp"), files.end()) << "the image is one of the document's files";

    write("ui/pic.bmp", bmp(8, 8, 0, 255, 255));
    const std::vector<UIDocumentSystem::Reloaded> reloaded = system.reloadChangedFiles({"ui/pic.bmp"});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_TRUE(reloaded[0].stylesheetOnly);
    EXPECT_EQ(system.getDocument(document.getEntity()), rml) << "an image change keeps the document";
    EXPECT_EQ(system.getLoadCount(document.getEntity()), 1u);

    ASSERT_TRUE(capture(frame, 3));
    EXPECT_TRUE(frame.is(70, 70, 0, 255, 255)) << "the <img> draws the replaced file";
    EXPECT_TRUE(frame.is(190, 70, 0, 255, 255)) << "the CSS background draws the replaced file";

    // the same file again: nothing to do (the stamps are current)
    EXPECT_TRUE(system.reloadChangedFiles({"ui/pic.bmp"}).empty());
}

TEST_F(CompatHotReloadTest, ReplacedFontReloadsTheDocumentWithTheNewFace){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument("ui/hot.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    UIDocumentSystem& system = documentSystem(scene);
    GpuEnvironment::drawFrames(2);
    Rml::ElementDocument* rml = system.getDocument(document.getEntity());
    ASSERT_NE(rml, nullptr);
    const float inter = widthOf(rml, "txt");
    ASSERT_GT(inter, 100.0f);
    for (const hc::Diagnostic& diagnostic : system.getDiagnostics(document.getEntity()))
        EXPECT_NE(diagnostic.code, hc::codes::UnknownFontFamily) << hc::toText(diagnostic);
    const std::vector<std::string> files = system.getDocumentFiles(document.getEntity());
    EXPECT_NE(std::find(files.begin(), files.end(), "ui/hot.ttf"), files.end()) << "the font is one of the document's files";

    // Orbitron is much wider than Inter
    replace("ui/hot.ttf", fs::path(JASPER_HTMLUI_TEST_SOURCE_DIR) / "fixtures/compat/pages/htmlui-imports/fonts/orbitron/Orbitron-700.ttf");
    const std::vector<UIDocumentSystem::Reloaded> reloaded = system.reloadChangedFiles({"ui/hot.ttf"});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_FALSE(reloaded[0].stylesheetOnly);
    ASSERT_TRUE(fixture.waitUntilLoaded());
    GpuEnvironment::drawFrames(2);
    Rml::ElementDocument* after = system.getDocument(document.getEntity());
    ASSERT_NE(after, nullptr);
    EXPECT_GT(widthOf(after, "txt"), inter * 1.15f) << "the text is laid out with the replaced font";
}

TEST_F(CompatHotReloadTest, SheetsRestyleAndTheDocumentReloads){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument("ui/hot.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    UIDocumentSystem& system = documentSystem(scene);
    Rml::ElementDocument* rml = system.getDocument(document.getEntity());
    Screen frame;
    ASSERT_TRUE(capture(frame, 3));
    ASSERT_TRUE(frame.is(310, 70, 0, 0, 255)) << "the @imported sheet";

    // the @imported sheet: a dependency of the document, restyled in place
    write("ui/more.css", std::string(importedCss) + "#imp { background: rgb(255, 255, 0); }\n");
    std::vector<UIDocumentSystem::Reloaded> reloaded = system.reloadChangedFiles({"ui/more.css"});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_TRUE(reloaded[0].stylesheetOnly);
    EXPECT_EQ(system.getDocument(document.getEntity()), rml);
    ASSERT_TRUE(capture(frame, 3));
    EXPECT_TRUE(frame.is(310, 70, 255, 255, 0));

    // the linked sheet
    write("ui/hot.css", std::string(documentCss) + "#pic { left: 400px; }\n");
    reloaded = system.reloadChangedFiles({"ui/hot.css"});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_TRUE(reloaded[0].stylesheetOnly);
    EXPECT_EQ(system.getDocument(document.getEntity()), rml);
    ASSERT_TRUE(capture(frame, 3));
    EXPECT_TRUE(frame.is(450, 70, 255, 0, 255)) << "the <img> moved";
    EXPECT_TRUE(frame.is(310, 70, 255, 255, 0)) << "the @imported sheet's change stays";

    // the document: reloaded in full, once
    std::string html = documentHtml;
    html.replace(html.find("</body>"), 7, "<div id=\"added\" style=\"position: absolute; left: 500px; top: 200px; width: 50px; "
                                          "height: 50px; background: lime\"></div></body>");
    write("ui/hot.html", html);
    reloaded = system.reloadChangedFiles({"ui/hot.html"});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_FALSE(reloaded[0].stylesheetOnly);
    ASSERT_TRUE(fixture.waitUntilLoaded());
    // (the count is of loads and stylesheet reloads, as for .rml documents)
    EXPECT_EQ(system.getLoadCount(document.getEntity()), 4u);
    ASSERT_TRUE(capture(frame, 3));
    EXPECT_TRUE(frame.is(525, 225, 0, 255, 0));
    // one summary line per load: two loads and two restyles
    EXPECT_LE(TestSystem::get().countLogs(0, "ui/hot.html: 0 errors"), 4u);
}
