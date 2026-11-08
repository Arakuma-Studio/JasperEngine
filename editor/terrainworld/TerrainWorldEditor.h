// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T4 R40, R43: the editor's side of Terrain Worlds that every route shares (the Create dialog, MCP,
// the panel): where a world's data comes from in the editor, making a new world as one undo step,
// and the macro colours of its layers.

#include "ecs/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine {
    class TerrainWorldRuntime;
    struct TerrainComponent;
    struct MeshComponent;
}

namespace jasperengine::editor {

    class Project;

    // Once, with the App's project: the runtime reads the project folder, and a scene in Play gets a
    // fresh runtime that never writes (R31)
    void installTerrainWorldHost(Project* project);
    // The editor's frame: the pump (an idle editor runs no engine frame) and the tiles' debounce
    void updateTerrainWorlds(double nowSeconds);

    struct TerrainWorldSettings {
        std::string name = "Terrain World";
        // assets-relative; ".terrainworld" is appended when missing
        std::string assetPath;
        int tileQuads = 512;
        float spacing = 1.0f;
        float maxHeight = 600.0f;
        float baseHeight = 0.0f;    // metres
        int gridWidth = 2, gridLength = 2;
        int originX = -1, originZ = -1;     // the grid's minimum tile
    };

    // R6: refuses what passes the world's limits on this platform, naming the limit
    bool validateTerrainWorldSettings(const TerrainWorldSettings& settings, std::string& error);
    // R43: GPU and CPU bytes the tiles would take resident, for the dialog and the dry run
    uint64_t predictTerrainWorldBytes(const TerrainWorldSettings& settings);
    // R40: the folder, then the GameObject (Transform, Mesh, Terrain, Terrain World, a Rigidbody
    // without colliders) as one undo step "Create Terrain World (Name)"
    Entity createTerrainWorld(Project* project, uint32_t sceneId, Entity parent, const TerrainWorldSettings& settings, std::string& error);

    // R30: while it lives, an undo or redo replaying a World's bytes makes no tile (the store's own
    // hook puts back the tiles a step made); nothing for a plain Terrain
    class TerrainWorldAutoAddPause {
    public:
        explicit TerrainWorldAutoAddPause(TerrainComponent& terrain);
        ~TerrainWorldAutoAddPause();
        TerrainWorldAutoAddPause(const TerrainWorldAutoAddPause&) = delete;
        TerrainWorldAutoAddPause& operator=(const TerrainWorldAutoAddPause&) = delete;

    private:
        TerrainWorldRuntime* runtime = nullptr;
        bool was = false;
    };

    // R52: base material then each layer, RGBA: a layer's tint over its colour map's mean
    std::vector<float> terrainWorldLayerColours(const TerrainComponent& terrain, const MeshComponent* mesh);

}
