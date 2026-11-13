// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Which languages a build ships (docs/specs/S87-title-localisation-interchange.md AC10's plan, R24,
// R25): Project Settings > Localization > Languages drops every other language's sidecar, PNG and
// meta with W-TITLE-LANGUAGE-DROPPED, the build's source sidecar lists only what ships, the
// project's file stays as it is, and an empty list ships everything.

#include <gtest/gtest.h>

#include "AppSettings.h"
#include "Project.h"
#include "support/EditorTestSupport.h"
#include "titleart/TitleArtIO.h"
#include "titleart/localisation/TitleLanguageExport.h"
#include "util/TitleArtBakeInfo.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include "support/PosixCompat.h"

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    constexpr const char* SIDECAR_HEAD =
        R"("format": "jasperengine.titleart.bakeinfo", "version": 1, "frame": {"x": 0, "y": 0, "w": 400, "h": 200},
           "origin": [0, 0], "anchors": [], "alpha_bounds": {"x": 0, "y": 0, "w": 400, "h": 200}, "source_hash": "00", "bake_version": 1)";

    std::string output(const std::string& file) {
        return R"("outputs": [{"id": "main", "file": ")" + file + R"(", "content": "all", "size": [400, 200]}])";
    }

    struct Fixture {
        bool ready = false;
        std::unique_ptr<Project> project;
        fs::path dir;
        std::string sourceSidecar;
    };

    Fixture& fixture() {
        static Fixture f = []() {
            Fixture out;
            test::ensureEngine();
            const fs::path root = fs::temp_directory_path() / ("jasper-title-shipping-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            const fs::path titles = out.dir / "assets" / "titles";
            fs::create_directories(titles);
            std::ofstream(out.dir / "project.yaml") << "name: titleshipping\n";
            std::ofstream(titles / "win.titleart") << titleart::writeTitleArt(titleart::makeNewTitleArt({800, 400, "WIN", ""}));
            out.sourceSidecar = std::string("{") + SIDECAR_HEAD + ", " + output("win.titleart.png") +
                                R"(, "languages": {"source": "en", "sets": {"de": "win.titleart.de.bake.json", "fr": null, "ja": "win.titleart.ja.bake.json"}}})";
            std::ofstream(titles / "win.titleart.bake.json") << out.sourceSidecar;
            std::ofstream(titles / "win.titleart.de.bake.json") << std::string("{") + SIDECAR_HEAD + ", " + output("win.titleart.de.png") + R"(, "language": "de"})";
            std::ofstream(titles / "win.titleart.ja.bake.json") << std::string("{") + SIDECAR_HEAD + ", " + output("win.titleart.ja.png") + R"(, "language": "ja"})";
            AppSettings::initialize();
            out.project.reset(new Project());
            test::useProjectPaths(out.project.get());
            out.ready = out.project->loadProject(out.dir, false);
            test::releaseAtExit(out.project.get());
            return out;
        }();
        if (f.project) test::useProjectPaths(f.project.get());
        return f;
    }

    std::string readAll(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

}

TEST(TitleLanguageShipping, DropsTheLanguagesTheProjectDoesNotList) {
    Fixture& f = fixture();
    ASSERT_TRUE(f.ready);
    f.project->getLocalizationSettings().languages = {"en", "DE", "fr"};
    const TitleLanguageShipping plan = planTitleLanguageShipping(f.project.get());
    EXPECT_EQ(plan.droppedFiles, (std::set<std::string>{"assets/titles/win.titleart.ja.bake.json", "assets/titles/win.titleart.ja.png",
                                                        "assets/titles/win.titleart.ja.png.meta"}));
    ASSERT_EQ(plan.warnings.size(), 1u);
    EXPECT_NE(plan.warnings[0].find("W-TITLE-LANGUAGE-DROPPED"), std::string::npos) << plan.warnings[0];
    EXPECT_NE(plan.warnings[0].find("win.titleart"), std::string::npos) << plan.warnings[0];
    EXPECT_NE(plan.warnings[0].find("ja"), std::string::npos) << plan.warnings[0];
    ASSERT_EQ(plan.sidecars.size(), 1u);
    EXPECT_EQ(plan.sidecars[0].first, "assets/titles/win.titleart.bake.json");
    TitleArtBakeInfo shipped;
    std::string error;
    ASSERT_TRUE(TitleArtBakeInfo::parse(plan.sidecars[0].second, shipped, error)) << error;
    EXPECT_EQ(shipped.languages.source, "en");
    std::vector<std::string> codes;
    for (const auto& set : shipped.languages.sets) codes.push_back(set.first);
    EXPECT_EQ(codes, (std::vector<std::string>{"de", "fr"})) << "the table without Japanese, in its order";
    EXPECT_EQ(readAll(f.dir / "assets/titles/win.titleart.bake.json"), f.sourceSidecar) << "the project's sidecar is unchanged";
}

TEST(TitleLanguageShipping, AnEmptyListShipsEverything) {
    Fixture& f = fixture();
    ASSERT_TRUE(f.ready);
    f.project->getLocalizationSettings().languages.clear();
    const TitleLanguageShipping plan = planTitleLanguageShipping(f.project.get());
    EXPECT_TRUE(plan.droppedFiles.empty());
    EXPECT_TRUE(plan.sidecars.empty());
    EXPECT_TRUE(plan.warnings.empty());
}

TEST(TitleLanguageShipping, AllListedShipsTheTableAsItIs) {
    Fixture& f = fixture();
    ASSERT_TRUE(f.ready);
    f.project->getLocalizationSettings().languages = {"en", "de", "fr", "ja"};
    const TitleLanguageShipping plan = planTitleLanguageShipping(f.project.get());
    EXPECT_TRUE(plan.droppedFiles.empty());
    EXPECT_TRUE(plan.sidecars.empty()) << "nothing to rewrite";
}
