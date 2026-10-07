// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A reloaded document reads its files as they are on disk now, while RmlUi stays
// initialised (another document of the scene, like the editor's open scenes, keeps it
// alive): RmlUi caches linked stylesheets by path until Rml::Shutdown, so before the fix an
// edited .css never showed until every document of every scene was gone.

#include "TestGoldenCommon.h"

#include "HtmlUI.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const char* reloadHtml = R"(<html>
<head>
    <title>reload</title>
    <link type="text/css" href="../common/base.css"/>
    <link type="text/css" href="reload.css"/>
</head>
<body>
    <div id="box"/>
</body>
</html>
)";

    std::string reloadCss(const std::string& color){
        return "body { position: absolute; left: 0; top: 0; width: 100%; height: 100%; margin: 0; }\n"
               "#box { position: absolute; left: 200px; top: 100px; width: 240px; height: 160px; background-color: " + color + "; }\n";
    }

    void writeFile(const std::string& path, const std::string& text){
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    }

}

TEST_F(FullPathGoldenTest, ReloadReadsEditedStyleSheet){
    // the fixtures' common/ is next to the written files: data://golden-reload/../common/base.css
    // resolves through the user data directory, so copy base.css there too
    const std::string root = TestSystem::get().getUserDataPath();
    std::filesystem::create_directories(root + "/golden-reload");
    std::filesystem::create_directories(root + "/common");
    std::filesystem::copy_file(std::string(JASPER_HTMLUI_TEST_SOURCE_DIR) + "/fixtures/common/base.css", root + "/common/base.css",
                               std::filesystem::copy_options::overwrite_existing);
    writeFile(root + "/golden-reload/reload.html", reloadHtml);
    writeFile(root + "/golden-reload/reload.css", reloadCss("#ff0000"));

    SceneFixture fixture;
    // keeps RmlUi initialised through the reloads
    fixture.addDocument("golden/orientation.html");
    UIDocument document = fixture.addDocument("data://golden-reload/reload.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    ASSERT_TRUE(document.isLoaded());

    Screen first;
    ASSERT_TRUE(capture(first));
    EXPECT_TRUE(first.is(320, 180, 255, 0, 0));
    EXPECT_TRUE(first.is(60, 35, 255, 0, 0));   // the other document

    // edit the stylesheet, reload the document
    writeFile(root + "/golden-reload/reload.css", reloadCss("#00ff00"));
    document.reload();
    Screen second;
    ASSERT_TRUE(capture(second, 2));
    ASSERT_TRUE(document.isLoaded());
    EXPECT_TRUE(second.is(320, 180, 0, 255, 0)) << "the reloaded document kept the old stylesheet";

    // the same through a filename change and back (what the Inspector and MCP do)
    writeFile(root + "/golden-reload/reload.css", reloadCss("#0000ff"));
    document.setFilename("");
    GpuEnvironment::drawFrames(1);
    document.setFilename("data://golden-reload/reload.html");
    Screen third;
    ASSERT_TRUE(capture(third, 2));
    EXPECT_TRUE(third.is(320, 180, 0, 0, 255));
    // the other document kept its own styles through it all
    EXPECT_TRUE(third.is(60, 35, 255, 0, 0));
    EXPECT_TRUE(HtmlUI::isInitialized());
}
