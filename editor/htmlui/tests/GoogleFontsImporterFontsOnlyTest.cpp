// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Google Fonts importer's fonts-only mode (docs/specs/S82-title-designer.md R57, AC27): the
// Title Designer's picker imports the chosen styles and the family's licence under the folder it
// names, with no CSS and no manifest entry, through the fixture server (no internet). The default
// mode is GoogleFontsImporterTest.cpp's, unchanged.

#include "ImporterTestCommon.h"

#include "htmlui/GoogleFontsImporter.h"

#include <algorithm>
#include <filesystem>

using namespace jasperengine::editor::htmlui;
using namespace jasperengine::editor::htmlui::test;

namespace {

    std::string interRegularAndBold() {
        GoogleFontsImporter::FamilyRequest request;
        std::string error;
        EXPECT_TRUE(GoogleFontsImporter::parseSpec("Inter:400,700", request, error)) << error;
        return GoogleFontsImporter::css2Url({request});
    }

    GoogleFontsImporter::Options fontsOnly() {
        GoogleFontsImporter::Options options;
        options.writeCss = false;
        options.writeManifest = false;
        options.dir = "TitleArt/Fonts/Google";
        return options;
    }

    // every .css file under the assets
    std::vector<std::string> cssFiles(const TempProject& project) {
        std::vector<std::string> found;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(project.assets(), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file() && it->path().extension() == ".css") found.push_back(it->path().generic_string());
        }
        return found;
    }

}

// Red control: running the default mode here (GoogleFontsImporter(importer) without the options)
// writes htmlui-imports/fonts/inter/*.ttf, a CSS and a manifest entry, failing every check
TEST(GoogleFontsImporterTest, FontsOnlyMode) {
    TempProject project;
    const std::vector<std::string> cssBefore = cssFiles(project);
    const bool manifestBefore = project.exists("assets/htmlui-imports/manifest.json");
    const std::string url = interRegularAndBold();

    RemoteImporter importer(project.target());
    const ImportedResource result = GoogleFontsImporter(importer, fontsOnly()).run(url, ImportLocation());
    ASSERT_TRUE(result.ok) << result.error << "\n" << dump(importer.diagnostics());

    // the TrueType files with the importer's names, and the licence, under the given folder
    for (const char* path : {"assets/TitleArt/Fonts/Google/inter/Inter-400.ttf", "assets/TitleArt/Fonts/Google/inter/Inter-700.ttf"}) {
        ASSERT_TRUE(project.exists(path)) << path;
        EXPECT_TRUE(ttfInfo(project.read(path)).valid) << path;
    }
    EXPECT_TRUE(project.exists("assets/TitleArt/Fonts/Google/inter/OFL.txt"));
    EXPECT_NE(std::find(result.written.begin(), result.written.end(), "TitleArt/Fonts/Google/inter/OFL.txt"), result.written.end());

    // no CSS, no manifest entry, nothing in the HTML UI import folder
    EXPECT_EQ(cssFiles(project), cssBefore);
    EXPECT_TRUE(result.entries.empty());
    EXPECT_EQ(project.exists("assets/htmlui-imports/manifest.json"), manifestBefore);
    EXPECT_EQ(project.manifest().find(url), nullptr);
    EXPECT_FALSE(project.exists("assets/htmlui-imports/fonts/inter/Inter-400.ttf"));
}

// The picker writes the files itself as one undo step: with `staging`, the importer only collects
// them. Red control: committing the staging in that mode writes the files and fails the first check.
TEST(GoogleFontsImporterTest, FontsOnlyModeCollectsIntoStaging) {
    TempProject project;
    ImportStaging staging;
    GoogleFontsImporter::Options options = fontsOnly();
    options.staging = &staging;

    RemoteImporter importer(project.target());
    const ImportedResource result = GoogleFontsImporter(importer, options).run(interRegularAndBold(), ImportLocation());
    ASSERT_TRUE(result.ok) << result.error << "\n" << dump(importer.diagnostics());

    EXPECT_FALSE(project.exists("assets/TitleArt/Fonts/Google/inter/Inter-400.ttf"));
    EXPECT_TRUE(staging.has("TitleArt/Fonts/Google/inter/Inter-400.ttf"));
    EXPECT_TRUE(staging.has("TitleArt/Fonts/Google/inter/Inter-700.ttf"));
    EXPECT_TRUE(staging.has("TitleArt/Fonts/Google/inter/OFL.txt"));
    for (const auto& file : staging.files()) EXPECT_EQ(file.first.find(".css"), std::string::npos) << file.first;
}
