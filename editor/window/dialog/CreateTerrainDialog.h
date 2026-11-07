// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T1 R58: Create Terrain (GameObject > 3D Object > Terrain, and the Terrain window's empty
// state). A preset of §6 Table 6 fills the sizes; Create makes the GameObject, its sizes, its
// height map and its collider as one undo step "Create Terrain (Name)". create_entity
// type=terrain still makes an empty terrain; both get applyNewTerrainDefaults (R91).

#pragma once

#include "ecs/Entity.h"

#include <cstdint>
#include <functional>
#include <string>

namespace jasperengine::editor {

    class Project;
    struct GenerationResult;

    class CreateTerrainDialog {
    public:
        enum class StartShape { Flat, FlatAtMiddle };

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
        static Entity create(Project* project, uint32_t sceneId, Entity parent, const Settings& settings, std::string& error);
        // T2 R61: Create, then settings.noisePreset run over the whole terrain at its height map's resolution, as
        // generate_terrain runs it with that preset and seed, folded into Create's undo step as "Create Terrain
        // (Rolling Hills)" once it ends; done runs then, on the main thread, once. NULL_ENTITY with error when
        // Create fails; a preset that cannot start leaves the flat terrain, with error and done not called.
        static Entity createGenerated(Project* project, uint32_t sceneId, Entity parent, const Settings& settings,
                                      std::function<void(const GenerationResult&)> done, std::string& error);
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

        void applyPreset(int index);
        void applyNoisePreset(const std::string& id);
    };

}
