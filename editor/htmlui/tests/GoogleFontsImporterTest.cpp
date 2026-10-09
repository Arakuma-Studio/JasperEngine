// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Google Fonts (phase-2 §3.13.3): css2 with weights, italics and ranges, the legacy css and icon
// APIs, unknown families, unavailable weights, licenses, the facade's specs; and the optional
// live test against fonts.googleapis.com (HTMLUI_LIVE_NETWORK=1).

#include "ImporterTestCommon.h"

#include "htmlui/GoogleFontsImporter.h"
#include "htmlui/HtmlUiImporters.h"
#include "htmlui/StylesheetImporter.h"

#include "htmlui/compat/UrlResolver.h"

using namespace jasperengine::editor::htmlui;
using namespace jasperengine::editor::htmlui::test;
using jasperengine::htmlui::compat::ImportEntry;
using Variant = GoogleFontsImporter::Variant;

namespace {

    ImportedResource importOne(const TempProject& project, const std::string& url, std::vector<compat::Diagnostic>* diagnostics = nullptr){
        RemoteImporter importer(project.target());
        ImportedResource result = importer.import(url);
        if (diagnostics)
            *diagnostics = importer.takeDiagnostics();
        return result;
    }

}

TEST(GoogleFonts, RecognizesItsUrls){
    EXPECT_TRUE(GoogleFontsImporter::isGoogleFontsUrl("https://fonts.googleapis.com/css2?family=Inter"));
    EXPECT_TRUE(GoogleFontsImporter::isGoogleFontsUrl("http://fonts.googleapis.com/css?family=Roboto"));
    EXPECT_TRUE(GoogleFontsImporter::isGoogleFontsUrl("//fonts.googleapis.com/icon?family=Material+Icons"));
    EXPECT_FALSE(GoogleFontsImporter::isGoogleFontsUrl("https://fonts.gstatic.com/s/inter/v1/x.ttf"));
    EXPECT_FALSE(GoogleFontsImporter::isGoogleFontsUrl("https://fonts.googleapis.com/other"));
}

TEST(GoogleFonts, ParsesRequests){
    const auto css2 = GoogleFontsImporter::parseRequest(
        "https://fonts.googleapis.com/css2?family=Roboto+Mono:ital,wght@0,400;1,700&family=Inter:wght@100..900&family=Press+Start+2P&display=swap");
    ASSERT_EQ(css2.size(), 3u);
    EXPECT_EQ(css2[0].family, "Roboto Mono");
    EXPECT_EQ(css2[0].variants, (std::vector<Variant>{{400, false}, {700, true}}));
    EXPECT_EQ(css2[1].family, "Inter");
    EXPECT_EQ(css2[1].rangeMin, 100);
    EXPECT_EQ(css2[1].rangeMax, 900);
    EXPECT_EQ(css2[2].family, "Press Start 2P");
    EXPECT_TRUE(css2[2].variants.empty());

    const auto legacy = GoogleFontsImporter::parseRequest("https://fonts.googleapis.com/css?family=Roboto:400,700i,bold|Open+Sans:italic");
    ASSERT_EQ(legacy.size(), 2u);
    EXPECT_EQ(legacy[0].variants, (std::vector<Variant>{{400, false}, {700, false}, {700, true}}));
    EXPECT_EQ(legacy[1].family, "Open Sans");
    EXPECT_EQ(legacy[1].variants, (std::vector<Variant>{{400, true}}));
}

TEST(GoogleFonts, SpecsBuildTheCss2Request){
    auto url = [](std::vector<std::string> specs){
        std::vector<GoogleFontsImporter::FamilyRequest> families;
        for (const std::string& spec : specs){
            GoogleFontsImporter::FamilyRequest request;
            std::string error;
            EXPECT_TRUE(GoogleFontsImporter::parseSpec(spec, request, error)) << error;
            families.push_back(request);
        }
        return GoogleFontsImporter::css2Url(families);
    };
    EXPECT_EQ(url({"Karla:400"}), "https://fonts.googleapis.com/css2?family=Karla:wght@400");
    EXPECT_EQ(url({"Inter:700,400"}), "https://fonts.googleapis.com/css2?family=Inter:wght@400;700");
    EXPECT_EQ(url({"Roboto Mono:400,400i,700italic"}), "https://fonts.googleapis.com/css2?family=Roboto+Mono:ital,wght@0,400;1,400;1,700");
    EXPECT_EQ(url({"Inter", "Orbitron:100..900"}), "https://fonts.googleapis.com/css2?family=Inter&family=Orbitron:wght@100..900");
    EXPECT_EQ(url({"Inter:ital,wght@1,700;0,400"}), "https://fonts.googleapis.com/css2?family=Inter:ital,wght@0,400;1,700");

    GoogleFontsImporter::FamilyRequest request;
    std::string error;
    EXPECT_FALSE(GoogleFontsImporter::parseSpec(":400", request, error));
    EXPECT_FALSE(GoogleFontsImporter::parseSpec("Inter:heavy", request, error));
    EXPECT_NE(error.find("heavy"), std::string::npos);
    EXPECT_FALSE(GoogleFontsImporter::parseSpec("Inter:900..100", request, error));
}

TEST(GoogleFonts, NamesAndLicenses){
    EXPECT_EQ(GoogleFontsImporter::familySlug("Press Start 2P"), "press-start-2p");
    EXPECT_EQ(GoogleFontsImporter::familyFileStem("Press Start 2P"), "PressStart2P");
    EXPECT_EQ(GoogleFontsImporter::licenseUrl("Press Start 2P"), "https://fonts.google.com/download/list?family=Press%20Start%202P");
    EXPECT_EQ(GoogleFontsImporter::licenseNameForFile("OFL.txt"), "SIL Open Font License 1.1");
    EXPECT_EQ(GoogleFontsImporter::licenseNameForFile("LICENSE.txt"), "Apache License 2.0");
    EXPECT_EQ(GoogleFontsImporter::licenseNameForFile("UFL.txt"), "Ubuntu Font License 1.0");
    EXPECT_EQ(GoogleFontsImporter::closestFamily("Interr", {"Inter", "Inter Tight", "Roboto"}), "Inter");
    EXPECT_EQ(GoogleFontsImporter::closestFamily("Zzzzzzzzzz", {"Inter", "Roboto"}), "");
}

TEST(GoogleFonts, Css2WeightsAndItalics){
    TempProject project;
    std::vector<compat::Diagnostic> diagnostics;
    const std::string url = "https://fonts.googleapis.com/css2?family=Inter:ital,wght@0,400;0,700;1,400&family=Press+Start+2P&display=swap";
    const ImportedResource result = importOne(project, url, &diagnostics);
    ASSERT_TRUE(result.ok) << result.error << "\n" << dump(diagnostics);

    // the fonts, named by family, weight and style, static TrueType with the right weight
    struct Expected { const char* path; int weight; bool italic; };
    for (const Expected& expected : {Expected{"assets/htmlui-imports/fonts/inter/Inter-400.ttf", 400, false},
                                     Expected{"assets/htmlui-imports/fonts/inter/Inter-700.ttf", 700, false},
                                     Expected{"assets/htmlui-imports/fonts/inter/Inter-400-italic.ttf", 400, true},
                                     Expected{"assets/htmlui-imports/fonts/press-start-2p/PressStart2P-400.ttf", 400, false}}){
        ASSERT_TRUE(project.exists(expected.path)) << expected.path;
        const TtfInfo info = ttfInfo(project.read(expected.path));
        EXPECT_TRUE(info.valid) << expected.path;
        EXPECT_FALSE(info.hasTable("fvar")) << expected.path;
        EXPECT_EQ(info.weight, expected.weight) << expected.path;
        EXPECT_EQ(info.italic, expected.italic) << expected.path;
    }

    // the manifest entry
    const compat::ImportManifest manifest = project.manifest();
    const ImportEntry* entry = manifest.find(url);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->kind, "google-fonts-css");
    EXPECT_EQ(entry->source, "Google Fonts");
    EXPECT_EQ(entry->path.compare(0, 31, "htmlui-imports/fonts/inter-pres"), 0) << entry->path;
    EXPECT_EQ(entry->path.substr(entry->path.size() - 4), ".css");
    EXPECT_EQ(entry->licenseName, "SIL Open Font License 1.1");
    EXPECT_EQ(entry->license.substr(entry->license.size() - 13), ".licenses.txt");    // two families: one combined file
    EXPECT_EQ(entry->sha256.size(), 64u);
    EXPECT_FALSE(entry->fetched.empty());
    for (const char* file : {"htmlui-imports/fonts/inter/Inter-400.ttf", "htmlui-imports/fonts/inter/OFL.txt",
                             "htmlui-imports/fonts/press-start-2p/OFL.txt"})
        EXPECT_NE(std::find(entry->files.begin(), entry->files.end(), file), entry->files.end()) << file;
    EXPECT_NE(project.read("assets/" + entry->license).find("==== Press Start 2P"), std::string::npos);

    // the local CSS: the same rules pointing at the local files
    const std::string css = project.read("assets/" + entry->path);
    const std::vector<CssFontFace> faces = findFontFaces(css);
    ASSERT_EQ(faces.size(), 4u) << css;
    for (const CssFontFace& face : faces){
        ASSERT_EQ(face.sources.size(), 1u);
        EXPECT_TRUE(face.sources[0].usable());
        EXPECT_EQ(face.sources[0].url.find("http"), std::string::npos) << face.sources[0].url;
        const std::string local = compat::UrlResolver::joinPath(entry->path, face.sources[0].url);
        EXPECT_TRUE(project.exists("assets/" + local)) << local;
    }
    EXPECT_NE(css.find("font-display: swap"), std::string::npos);   // the other descriptors stay

    // notes: imported and the license
    EXPECT_EQ(withCode(diagnostics, "HTMLUI-IMP-0004").size(), 1u) << dump(diagnostics);
    EXPECT_EQ(withCode(diagnostics, "HTMLUI-IMP-0008").size(), 1u) << dump(diagnostics);
    EXPECT_TRUE(withCode(diagnostics, "HTMLUI-IMP-0002").empty()) << dump(diagnostics);
}

TEST(GoogleFonts, RangesExpandToStaticInstances){
    TempProject project;
    const ImportedResource result = importOne(project, "https://fonts.googleapis.com/css2?family=Orbitron:wght@400..900");
    ASSERT_TRUE(result.ok) << result.error;
    for (int weight = 400; weight <= 900; weight += 100){
        const std::string path = "assets/htmlui-imports/fonts/orbitron/Orbitron-" + std::to_string(weight) + ".ttf";
        ASSERT_TRUE(project.exists(path)) << path;
        EXPECT_EQ(ttfInfo(project.read(path)).weight, weight);
    }
    // one family: its own license file
    const ImportEntry* entry = result.entry();
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->license, "htmlui-imports/fonts/orbitron/OFL.txt");
}

TEST(GoogleFonts, LegacyCssApiAndMissingVariants){
    TempProject project;
    std::vector<compat::Diagnostic> diagnostics;
    // Press Start 2P has no 700: the legacy API leaves it out silently, the importer says so
    const ImportedResource result = importOne(project, "https://fonts.googleapis.com/css?family=Roboto+Mono:400,700i|Press+Start+2P:400,700",
                                              &diagnostics);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(project.exists("assets/htmlui-imports/fonts/roboto-mono/RobotoMono-700-italic.ttf"));
    EXPECT_TRUE(project.exists("assets/htmlui-imports/fonts/press-start-2p/PressStart2P-400.ttf"));
    EXPECT_FALSE(project.exists("assets/htmlui-imports/fonts/press-start-2p/PressStart2P-700.ttf"));
    const auto unavailable = withCode(diagnostics, "HTMLUI-IMP-0006");
    ASSERT_EQ(unavailable.size(), 1u) << dump(diagnostics);
    EXPECT_NE(unavailable[0].message.find("700"), std::string::npos);
    EXPECT_EQ(unavailable[0].severity, compat::Severity::Warning);
    // Roboto Mono is Apache-licensed
    EXPECT_NE(result.entry()->licenseName.find("Apache License 2.0"), std::string::npos);
}

TEST(GoogleFonts, Css2UnavailableWeightImportsTheNearest){
    TempProject project;
    std::vector<compat::Diagnostic> diagnostics;
    const std::string url = "https://fonts.googleapis.com/css2?family=Press+Start+2P:wght@700";
    const ImportedResource result = importOne(project, url, &diagnostics);
    ASSERT_TRUE(result.ok) << result.error << dump(diagnostics);
    EXPECT_TRUE(project.exists("assets/htmlui-imports/fonts/press-start-2p/PressStart2P-400.ttf"));
    const auto unavailable = withCode(diagnostics, "HTMLUI-IMP-0006");
    ASSERT_EQ(unavailable.size(), 1u) << dump(diagnostics);
    EXPECT_NE(unavailable[0].message.find("offers 400"), std::string::npos) << unavailable[0].message;
    // the document's URL resolves
    EXPECT_NE(project.manifest().find(url), nullptr);
}

TEST(GoogleFonts, UnknownFamilySuggestsTheClosest){
    TempProject project;
    std::vector<compat::Diagnostic> diagnostics;
    const ImportedResource result = importOne(project, "https://fonts.googleapis.com/css2?family=Interr:wght@400", &diagnostics);
    EXPECT_FALSE(result.ok);
    const auto failed = withCode(diagnostics, "HTMLUI-IMP-0002");
    ASSERT_EQ(failed.size(), 1u) << dump(diagnostics);
    EXPECT_NE(failed[0].message.find("unknown Google Fonts family 'Interr'"), std::string::npos) << failed[0].message;
    EXPECT_NE(failed[0].suggestion.find("'Inter'"), std::string::npos) << failed[0].suggestion;
    // nothing written
    EXPECT_FALSE(project.exists("assets/htmlui-imports/manifest.json"));
    EXPECT_FALSE(fs::exists(project.assets() / "htmlui-imports"));
}

TEST(GoogleFonts, IconApi){
    TempProject project;
    const ImportedResource result = importOne(project, "https://fonts.googleapis.com/icon?family=Material+Icons");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(project.exists("assets/htmlui-imports/fonts/material-icons/MaterialIcons-400.ttf"));
    // the icon class stays in the local CSS
    EXPECT_NE(project.read("assets/" + result.entry()->path).find(".material-icons"), std::string::npos);
}

TEST(GoogleFonts, LicenseThatCannotBeFetchedIsUnknown){
    TempProject project;
    std::vector<compat::Diagnostic> diagnostics;
    const ImportedResource result = importOne(project, "https://fonts.googleapis.com/css2?family=Unlicensed+Sans", &diagnostics);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.entry()->licenseName, "unknown");
    EXPECT_TRUE(result.entry()->license.empty());
    const auto notes = withCode(diagnostics, "HTMLUI-IMP-0008");
    ASSERT_EQ(notes.size(), 1u) << dump(diagnostics);
    EXPECT_NE(notes[0].message.find("could not be fetched"), std::string::npos);
}

TEST(GoogleFonts, FontDownloadFailureWritesNothing){
    TempProject project;
    // the fonts host answers 404 for everything: the CSS came, the TTFs did not
    std::map<std::string, std::string> rewrites = sharedServer().rewrites();
    rewrites["https://fonts.gstatic.com"] = sharedServer().base() + "/status/404/";
    ScopedRewrite scoped(rewrites);
    std::vector<compat::Diagnostic> diagnostics;
    const ImportedResource result = importOne(project, "https://fonts.googleapis.com/css2?family=Inter:wght@400", &diagnostics);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(withCode(diagnostics, "HTMLUI-IMP-0002").empty()) << dump(diagnostics);
    EXPECT_FALSE(fs::exists(project.assets() / "htmlui-imports"));
}

TEST(GoogleFonts, FacadeWritesFontsAndFontFaceCss){
    TempProject project(false);
    const ImportResult result = importGoogleFonts(project.root(), {"Karla:400", "Roboto Mono:400,400i"}, "assets/ui/imported");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.fontFaceCss.compare(0, 25, "assets/ui/imported/fonts/"), 0) << result.fontFaceCss;
    EXPECT_TRUE(project.exists(result.fontFaceCss));
    EXPECT_TRUE(project.exists("assets/ui/imported/fonts/karla/Karla-400.ttf"));
    EXPECT_TRUE(project.exists("assets/ui/imported/fonts/roboto-mono/RobotoMono-400-italic.ttf"));
    EXPECT_NE(std::find(result.written.begin(), result.written.end(), "assets/ui/imported/fonts/karla/Karla-400.ttf"), result.written.end());
    ASSERT_EQ(result.replaced.size(), 1u);
    EXPECT_EQ(result.replaced[0].first, "https://fonts.googleapis.com/css2?family=Karla:wght@400&family=Roboto+Mono:ital,wght@0,400;1,400");
    EXPECT_EQ(result.replaced[0].second, result.fontFaceCss);
    // the manifest stays in htmlui-imports
    EXPECT_TRUE(project.exists("assets/htmlui-imports/manifest.json"));

    const ImportResult bad = importGoogleFonts(project.root(), {"Inter:heavy"}, "");
    EXPECT_FALSE(bad.ok);
    ASSERT_EQ(bad.failed.size(), 1u);
    EXPECT_EQ(bad.failed[0].first, "Inter:heavy");

    const ImportResult outside = importGoogleFonts(project.root(), {"Inter"}, "../elsewhere");
    EXPECT_FALSE(outside.ok);
    EXPECT_NE(outside.error.find("not inside the project's assets"), std::string::npos) << outside.error;
}

TEST(GoogleFonts, LiveNetwork){
    const char* live = std::getenv("HTMLUI_LIVE_NETWORK");
    if (!live || std::string(live) != "1")
        GTEST_SKIP() << "set HTMLUI_LIVE_NETWORK=1 to import from fonts.googleapis.com";
    ScopedRewrite none({});
    GoogleFontsImporter::clearSessionCache();
    TempProject project(false);
    const std::string url = "https://fonts.googleapis.com/css2?family=Inter:wght@400;700&family=Press+Start+2P&display=swap";
    std::vector<compat::Diagnostic> diagnostics;
    const ImportedResource result = importOne(project, url, &diagnostics);
    ASSERT_TRUE(result.ok) << result.error << "\n" << dump(diagnostics);
    for (const auto& expected : {std::make_pair("assets/htmlui-imports/fonts/inter/Inter-400.ttf", 400),
                                 std::make_pair("assets/htmlui-imports/fonts/inter/Inter-700.ttf", 700),
                                 std::make_pair("assets/htmlui-imports/fonts/press-start-2p/PressStart2P-400.ttf", 400)}){
        ASSERT_TRUE(project.exists(expected.first)) << expected.first;
        const TtfInfo info = ttfInfo(project.read(expected.first));
        EXPECT_TRUE(info.valid);
        EXPECT_FALSE(info.hasTable("fvar")) << expected.first << " is a variable font";
        EXPECT_FALSE(info.hasTable("gvar"));
        EXPECT_EQ(info.weight, expected.second);
    }
    EXPECT_TRUE(project.exists("assets/htmlui-imports/fonts/inter/OFL.txt"));
    EXPECT_TRUE(project.exists("assets/htmlui-imports/fonts/press-start-2p/OFL.txt"));
    const compat::ImportManifest manifest = project.manifest();
    const ImportEntry* entry = manifest.find(url);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->licenseName, "SIL Open Font License 1.1");
}
