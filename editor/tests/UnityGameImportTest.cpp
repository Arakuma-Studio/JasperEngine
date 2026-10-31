// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The importer on the real game's 18 Particle Systems (particles spec 4.1, 9). The game is not
// part of the repository (its WarFX systems are Asset Store content): the test runs only when a
// clone is at /tmp/gamesrc/Minigames or JASPER_UNITY_GAME_DIR names one, and writes into a temp
// folder. Each file imports, every system becomes a version 2 system, no module the game uses is
// unsupported, and the notes are the expected ones.

#include <gtest/gtest.h>

#include "unity/UnityParticleImport.h"

#include <cstdlib>
#include <map>
#include <random>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

    fs::path gameDir() {
        if (const char* env = std::getenv("JASPER_UNITY_GAME_DIR")) return fs::path(env);
        return fs::path("/tmp/gamesrc/Minigames");
    }

    struct GameFile {
        const char* path;
        size_t systems;
    };

    // the files holding the 18 systems, and how many each holds
    const GameFile GAME_FILES[] = {
        {"Assets/Scenes/A.unity", 2},                                // PlayerAppereance, ExplosionParticles
        {"Assets/Scenes/B.unity", 2},                                // Start/EndParticles
        {"Assets/Scenes/D.unity", 5},                                // Eyes x2, Rush/Ambush x3
        {"Assets/Scenes/E.unity", 1},                                // the WarFX flamethrower instance
        {"Assets/Prefabs/CollectedParticles.prefab", 1},
        {"Assets/Prefabs/GameOverMenu.prefab", 1},                   // confetti in a UIParticle
        {"Assets/Prefabs/I - Invaders/InvadersExplosion.prefab", 2}, // the root and its Explosion
        {"Assets/Prefabs/I - Invaders/Player.prefab", 4},            // FirePoints
    };

    // notes a system of the game may have: approximations the spec lists for it
    const std::set<std::string> EXPECTED_NOTES = {
        "ui-particle", "shader-approximated", "mesh-missing", "noise-pattern", "auto-destruct", "culling-mode",
        "material-missing", "texture-format"};

}

TEST(UnityGameImportTest, TheGamesEighteenSystems) {
    const fs::path game = gameDir();
    std::error_code ec;
    if (!fs::is_directory(game / "Assets", ec)) {
        GTEST_SKIP() << "no game clone at " << game << " (set JASPER_UNITY_GAME_DIR)";
    }
    std::random_device rd;
    const fs::path out = fs::temp_directory_path() / ("jasper-unity-game-" + std::to_string(rd()));
    fs::create_directories(out);

    size_t total = 0;
    bool uiParticle = false;
    for (const GameFile& file : GAME_FILES) {
        if (!fs::is_regular_file(game / file.path, ec)) {
            ADD_FAILURE() << "missing in the clone: " << file.path;
            continue;
        }
        unity::ImportOptions options;
        options.file = game / file.path;
        options.engineProject = out;
        options.dest = "fx/" + fs::path(file.path).stem().string();
        const unity::ImportResult result = unity::importParticles(options);
        ASSERT_TRUE(result.ok) << file.path << ": " << result.error;
        EXPECT_FALSE(result.prefabs.empty()) << file.path;
        const Json& systems = result.report["systems"];
        EXPECT_EQ(systems.size(), file.systems) << file.path << "\n" << result.report.dump(2);
        total += systems.size();
        for (const Json& system : systems) {
            for (const auto& module : system["modules"].items()) {
                EXPECT_NE(module.value(), "unsupported") << file.path << " " << system["path"] << " " << module.key();
            }
            for (const Json& note : system["notes"]) {
                const std::string code = note["code"];
                EXPECT_TRUE(EXPECTED_NOTES.count(code)) << file.path << " " << system["path"] << ": " << code << ": " << note["message"];
                uiParticle = uiParticle || code == "ui-particle";
            }
        }
    }
    EXPECT_EQ(total, 18u);
    EXPECT_TRUE(uiParticle) << "GameOverMenu's confetti is drawn by UIParticle";
    fs::remove_all(out, ec);
}
