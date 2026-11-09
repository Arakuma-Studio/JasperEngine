// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// game.json v1 (docs/specs/S30-shipping.md D4): what a prebuilt player knows of the game it runs,
// replacing the generated init() of a Compile build: the scenes and prefabs, the window, the
// canvas and everything else Project Settings put in that init. The editor fills one from the
// project (writeGameConfig) and writes toJson(); the player reads it with fromJson() and
// applies it. One schema for both.

#include "Export.h"
#include "Engine.h"
#include "navigation/NavTypes.h"
#include "registry/PhysicsLayers.h"
#include "registry/SortingLayers.h"
#include "render/AtmosphereQuality.h"
#include "render/LightingTiers.h"
#include "render/shadow/ShadowTiers.h"
#include "texture/MaterialPbr.h"
#include "device/DeviceSettings.h"
#include "pixelart/ScreenTransition.h"

#include "json.hpp"

#include <array>
#include <map>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::player {

    struct GameScene {
        uint32_t id = 0;
        std::string name;
        std::string file;                  // "scenes/<id>.jscn"
        std::vector<uint32_t> involved;    // the stack's scenes (made with it)
        std::vector<uint32_t> active;      // the ones drawn: the scene itself first, then its layers
    };

    struct GamePrefab {
        uint32_t id = 0;
        std::string name;                  // its path without the extension
        std::string guid;
        std::string file;                  // "prefabs/<id>.jpfb"
    };

    struct PlayerConfig {
        static constexpr int SCHEMA_VERSION = 1;

        std::string name;
        std::string appId;
        std::string version;
        uint32_t startScene = 0;
        std::vector<GameScene> scenes;
        std::vector<GamePrefab> prefabs;

        // window
        int windowMode = 0;                // 0 windowed, 1 maximized, 2 fullscreen
        int windowWidth = 960;
        int windowHeight = 540;
        bool resizable = true;
        std::string title;
        bool vsync = true;
        // Project Settings > Graphics > Dynamic Batching
        bool dynamicBatching = true;
        std::string icon;                  // content-relative PNG ("icon.png"), the window icon
        std::string storageId;

        // canvas
        unsigned int canvasWidth = 1000;
        unsigned int canvasHeight = 480;
        Scaling scaling = Scaling::FITWIDTH;
        TextureStrategy textureStrategy = TextureStrategy::NONE;
        // Project Settings > Graphics > Pixels Per Unit (S41)
        int pixelsPerUnit = 1;
        // Project Settings > Graphics > Default Filter Mode is Point (S42 R3)
        bool defaultFilterPoint = false;
        // Project Settings > Graphics > Bitmap Text Cache (S51), 0 = the platform's default
        int bitmapTextCacheMiB = 0;

        // loading
        uint32_t loadingScene = 0;         // 0: none
        float loadingDelay = 0.0f;
        float loadingTimeout = 5.0f;
        bool asyncLoading = false;

        ThreadingSettings threading;
        uint32_t particleWorldBudget = 0;   // 0: the platform's default
        std::array<AtmosphereQuality, 3> atmosphereQuality = {AtmosphereQuality::HIGH, AtmosphereQuality::MEDIUM, AtmosphereQuality::LOW};
        std::array<FoliageQuality, 3> foliageQuality = {FoliageQuality::HIGH, FoliageQuality::HIGH, FoliageQuality::HIGH};
        bool foliageQualityUncapped = true;
        // Quality > Foliage Distance (desktop, mobile, web)
        std::array<float, 3> foliageDistanceScale = {1.0f, 1.0f, 1.0f};
        float inputDeadzone = 0.15f;
        std::string inputActions;
        nav::NavigationSettings navigation;
        // Project Settings > Voxels (T6 R7): what the project lowers of the platform's limits, 0: nothing
        int voxelMaxDimension = 0;
        int voxelCpuBudgetMB = 0;
        int voxelMeshBudgetMB = 0;
        std::array<Engine::TexturePlatformDefaults, 3> textureDefaults;
        int htmlUiMsaa = 4;
        int glyphAtlasMiB = 0;
        bool sdfPrimitives = true;
        bool transformAwareText = true;
        PbrTier mobilePbrTier = PbrTier::REDUCED;
        // Project Settings > Graphics > Light Mode
        LightMode lightMode = LightMode::AUTOMATIC;
        // Project Settings > Graphics > Screen Transitions (S48): "transitions", written when not the defaults
        pixelart::TransitionDefaults transitions;
        // per ShadowTiers platform
        std::array<ShadowTier, ShadowTiers::PLATFORM_COUNT> shadowTiers{};
        // Project Settings > Quality > Device Tiers, the LOD biases, Thermal Response and the
        // upload budget (spec S56): "device", what differs from the defaults
        DeviceSettings device;
        // T3 R2, R34: terraincore::TerrainQuality by Engine::Platform (absent: High), and the tiers the
        // build ships, which the tier a terrain asks for clamps to
        std::array<int, 6> terrainQualityTiers{};
        uint32_t terrainShippedTiers = 7;
        std::vector<std::string> tags;
        std::vector<std::string> layerNames;
        // Sorting Layers and Graphics > Transparency Sort (spec S52 R40); empty: Default alone
        std::vector<SortingLayerInfo> sortingLayers;
        TransparencySortMode transparencySortMode = TransparencySortMode::DEFAULT;
        Vector3 transparencySortAxis = Vector3(0.0f, 1.0f, 0.0f);
        std::array<uint32_t, MAX_LAYERS> collision3D{};
        std::array<uint32_t, MAX_LAYERS> collision2D{};

        // Player > Logging and Crash Handling (a Compile build's macros)
        std::string logLevel;
        bool logFile = true;
        bool logBacktrace = false;
        bool crashHandler = true;
        bool crashLogTail = true;
        bool crashDumpWithHeap = false;
        bool hangWatchdog = true;
        int hangMs = 5000;
        bool crashWebOverlay = true;

        // Project Settings > Audio (a Compile build's generated AudioSystem::configure and
        // AudioMixer::load): the defaults are AudioConfig's, the mixer asset-relative ("" = none)
        std::string audioMixer;
        int audioMaxRealVoices = 32;
        int audioMaxVoices = 256;
        int audioSeed = 1;
        int audioSampleRate = 0;
        std::string audioOutputStage = "roundoff";
        std::string audioIosSession = "default";
        // the group music plays through when nothing names one ("" = Master)
        std::string audioMusicGroup;

        // the project's native plugins with code for other platforms only (the Compile build's
        // NativeBridge::setPlatformOnlyPlugins): a call to one fails with "unavailable", and its
        // Lua mock answering logs a warning
        std::map<std::string, std::string> nativePlatformOnly;

        PlayerConfig();

        nlohmann::json toJson() const;
        // false (with a message saying what to do) when it is not a game.json this player reads
        static bool fromJson(const nlohmann::json& json, PlayerConfig& out, std::string& error);
        static bool parse(const std::string& text, PlayerConfig& out, std::string& error);

        const GameScene* findScene(uint32_t id) const;
        const GameScene* findScene(const std::string& nameOrId) const;
    };

    // The player's audio as Play and a Compile build have it: the voices and sample rate, then the
    // project's mixer (before the first scene loads)
    JASPERENGINE_API void applyAudio(const PlayerConfig& game);

}
