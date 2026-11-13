// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The Game view's fixed resolutions (ui-scaling-text spec 9, 14): presets, "WxH" / "WxH@dpi",
// custom names, the limits, and the custom ones kept in the editor workspace. The render size
// and Screen.dpi in Play are covered end to end by tools/mcp/tests/test_ui_scaling.py.

#include <gtest/gtest.h>
#include "Workspace.h"
#include "window/GameResolutions.h"

#include <filesystem>
#include <fstream>
#include "support/PosixCompat.h"

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

TEST(GameViewResolution, PresetsKeepTheirDpiAndHint) {
    GameResolution resolution;
    std::string error;
    ASSERT_TRUE(findGameResolution("2400x1080", {}, resolution, error)) << error;
    EXPECT_EQ(resolution.width, 2400);
    EXPECT_EQ(resolution.height, 1080);
    EXPECT_FLOAT_EQ(resolution.dpi, 400.0f);
    EXPECT_EQ(resolution.note, "20:9 phone");
    EXPECT_NE(gameResolutionLabel(resolution).find("400 dpi"), std::string::npos) << gameResolutionLabel(resolution);

    // every built-in one is found by its name, landscape and portrait
    bool portrait = false;
    for (const GameResolution& preset : builtInGameResolutions()) {
        GameResolution found;
        ASSERT_TRUE(findGameResolution(preset.name, {}, found, error)) << preset.name;
        EXPECT_EQ(found, preset);
        portrait = portrait || preset.height > preset.width;
    }
    EXPECT_TRUE(portrait);
}

TEST(GameViewResolution, SizesWithAndWithoutDpi) {
    GameResolution resolution;
    std::string error;
    ASSERT_TRUE(findGameResolution("1440x1080", {}, resolution, error)) << error;
    EXPECT_EQ(resolution.name, "1440x1080");
    EXPECT_EQ(resolution.width, 1440);
    EXPECT_EQ(resolution.height, 1080);
    EXPECT_FLOAT_EQ(resolution.dpi, 0.0f) << "0: the host's dpi";

    ASSERT_TRUE(findGameResolution("800x600@160", {}, resolution, error)) << error;
    EXPECT_EQ(resolution.width, 800);
    EXPECT_EQ(resolution.height, 600);
    EXPECT_FLOAT_EQ(resolution.dpi, 160.0f);

    // a preset's size given with another dpi takes that dpi
    ASSERT_TRUE(findGameResolution("2400x1080@300", {}, resolution, error)) << error;
    EXPECT_FLOAT_EQ(resolution.dpi, 300.0f);

    for (const char* bad : {"", "free", "1920", "1920x", "x1080", "8x8", "20000x100", "100x100@5000", "100x100@", "100x100junk", "-5x100"}) {
        error.clear();
        EXPECT_FALSE(findGameResolution(bad, {}, resolution, error)) << bad;
        EXPECT_NE(error.find("WxH"), std::string::npos) << bad << ": " << error;
    }
    EXPECT_TRUE(isValidGameResolution(16, 16, 0.0f));
    EXPECT_TRUE(isValidGameResolution(16384, 16384, 2000.0f));
    EXPECT_FALSE(isValidGameResolution(15, 100, 0.0f));
    EXPECT_FALSE(isValidGameResolution(100, 100, -1.0f));
}

TEST(GameViewResolution, CustomNamesAreFound) {
    const std::vector<GameResolution> custom = {{"Steam Deck", 1280, 800, 206.0f, ""}};
    GameResolution resolution;
    std::string error;
    ASSERT_TRUE(findGameResolution("Steam Deck", custom, resolution, error)) << error;
    EXPECT_EQ(resolution.width, 1280);
    EXPECT_EQ(resolution.height, 800);
    EXPECT_FLOAT_EQ(resolution.dpi, 206.0f);
    EXPECT_FALSE(findGameResolution("Steam Deck", {}, resolution, error));
    EXPECT_FALSE(findGameResolution("Deck", custom, resolution, error));
    EXPECT_NE(error.find("\"Steam Deck\""), std::string::npos) << "the error lists the custom ones: " << error;
}

// the custom resolutions live in the editor workspace (.jasperengine/user/workspace.yaml);
// entries a hand edit broke are dropped
TEST(GameViewResolution, WorkspaceKeepsTheCustomOnes) {
    const fs::path project = fs::temp_directory_path() / ("jasper-game-resolutions-" + std::to_string(::getpid()));
    fs::remove_all(project);
    fs::create_directories(Workspace::getUserDirectory(project));
    {
        std::ofstream file(Workspace::getUserDirectory(project) / "workspace.yaml");
        file << "selectedScene: 1\n"
                "gameResolutions:\n"
                "  - {name: Steam Deck, width: 1280, height: 800, dpi: 206}\n"
                "  - {name: Square, width: 1024, height: 1024}\n"
                "  - {name: Broken, width: 5, height: 800}\n"
                "  - {width: 640, height: 480}\n";
    }
    Workspace workspace;
    workspace.load(project);
    const std::vector<GameResolution>& custom = workspace.getCustomResolutions();
    ASSERT_EQ(custom.size(), 2u);
    EXPECT_EQ(custom[0].name, "Steam Deck");
    EXPECT_FLOAT_EQ(custom[0].dpi, 206.0f);
    EXPECT_EQ(custom[1].name, "Square");
    EXPECT_FLOAT_EQ(custom[1].dpi, 0.0f);

    // a workspace without the section loads none (also after one that had them)
    {
        std::ofstream file(Workspace::getUserDirectory(project) / "workspace.yaml");
        file << "selectedScene: 1\n";
    }
    workspace.load(project);
    EXPECT_TRUE(workspace.getCustomResolutions().empty());
    fs::remove_all(project);
}
