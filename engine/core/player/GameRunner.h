// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What every game does at start (docs/specs/S98-editor-tooling-gaps.md D1): apply game.json's
// settings, register its scene stacks and prefabs, built from their cooked files
// (CookedSceneLoader), and load the start scene. The prebuilt player calls it with no hooks; a
// Compile build's generated main.cpp hands over its C++ scripts (scene_scripts.cpp), the only
// code the editor generates for a game.

#include "Export.h"
#include "manager/ObjectManager.h"
#include "player/PlayerConfig.h"

#include <string>

namespace jasperengine {
    class Scene;
}

namespace jasperengine::player {

    struct ProjectHooks {
        // a scene's scripts once it is built (instances, then init and onEnable) and before it
        // goes; null: its Lua scripts only (LuaBinding::initializeLuaScripts / cleanupLuaScripts)
        void (*initScripts)(Scene* scene) = nullptr;
        void (*cleanupScripts)(Scene* scene) = nullptr;
        // the C++ scripts of GameObjects instantiated, destroyed and moved while the game runs
        ObjectManager::ScriptHook initEntityScripts = nullptr;
        ObjectManager::ScriptHook cleanupEntityScripts = nullptr;
        ObjectManager::ScriptMoveHook moveEntityScripts = nullptr;
    };

    // game.json under `contentUrl` ("content://", or a folder path ending in '/'); false with a
    // message saying what is wrong. A content-manifest.json beside it gives StrippedAssets its list.
    JASPERENGINE_API bool readGameConfig(const std::string& contentUrl, PlayerConfig& out, std::string& error);

    // The settings, before any scene: what game.json holds besides its scenes and prefabs
    JASPERENGINE_API void applySettings(const PlayerConfig& game);

    // applySettings, then the scene stacks and prefabs from the cooked files game.json names
    // (relative to `contentUrl`), then the start scene (--scene, else game.json's)
    JASPERENGINE_API void runGame(const PlayerConfig& game, const ProjectHooks& hooks = {}, const std::string& contentUrl = "content://");

}
