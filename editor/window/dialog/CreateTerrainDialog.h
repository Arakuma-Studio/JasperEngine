// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T1 R58: Create Terrain (GameObject > 3D Object > Terrain, and the Terrain window's empty
// state). A preset of §6 Table 6 fills the sizes; Create makes the GameObject, its sizes, its
// height map and its collider as one undo step "Create Terrain (Name)". create_entity
// type=terrain still makes an empty terrain; both get applyNewTerrainDefaults (R91).

#pragma once

#include "ecs/Entity.h"
#include "terrain/TerrainRawIO.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;
    struct GenerationResult;

    class CreateTerrainDialog {
    public:
        enum class StartShape { Flat, FlatAtMiddle, ImportRaw };

        struct Settings {
            std::string name = "Terrain";
            int preset = 2;             // Table 6's Small
            float size = 256.0f;
            float height = 64.0f;
            int heightmap = 512;
            int control = 512;
            int detail = 512;
            float meshResolution = 32.0f;
            int rootGrid = 2;
            int levels = 4;
            StartShape shape = StartShape::Flat;
            float baseHeight = 0.0f;    // metres, Flat
            // ImportRaw: the decoded height file (TerrainRawIO), square and heightmap texels a side
            HeightImport imported;
            // An image of the assets for the material's base color, the Base row of the Terrain window's
            // layers: it covers the whole terrain with no blend map; empty keeps the material's own
            std::string baseLayer;
            bool collider = true;
            // T2 R61: a Noise Graph preset run on the new terrain after Create (its own undo step); empty: none
            std::string noisePreset;
            uint32_t seed = 1;
        };

        static CreateTerrainDialog& instance();

        void open(Project* project, uint32_t sceneId, Entity parent);
        void show();
        bool opened() const { return isOpen; }

        // The undo step Create makes; the new GameObject, or NULL_ENTITY with error
        // heights: the height map's RED16 bytes (T2 R61's generated ones), else flat; stepName: else
        // "Create Terrain (Name)"
        static Entity create(Project* project, uint32_t sceneId, Entity parent, const Settings& settings, std::string& error,
                             const std::vector<uint8_t>* heights = nullptr, const std::string& stepName = {});
        // T2 R61: settings.noisePreset run over the new height map on a job, as generate_terrain runs it with that
        // preset and seed, then Create with those heights: one undo step, "Create Terrain (Rolling Hills)". done
        // runs on the main thread, once, with the new GameObject (NULL_ENTITY and the result's error on a failure).
        // False with error, and done not called, when the preset cannot run.
        static bool createGenerated(Project* project, uint32_t sceneId, Entity parent, const Settings& settings,
                                    std::function<void(Entity, const GenerationResult&)> done, std::string& error);
        // What the maps take on the CPU and GPU, and the platforms' warnings
        static std::string memoryLine(const Settings& settings);

    private:
        bool isOpen = false;
        bool justOpened = false;
        Project* project = nullptr;
        uint32_t sceneId = 0;
        Entity parent = NULL_ENTITY;
        Settings settings;
        std::string error;
        // Start Shape > Import Raw...: the chosen file and its import fields (R61's, shared)
        std::string importPath;
        std::vector<uint8_t> importBytes;
        HeightImportOptions importOptions;
        bool importSetsHeight = false;

        void applyPreset(int index);
        void applyNoisePreset(const std::string& id);
        void chooseImportFile();
        void useImportedSize();
    };

}
