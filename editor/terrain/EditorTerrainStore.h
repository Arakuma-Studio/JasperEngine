// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "object/TerrainEdit.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor{

    class Project;

    // T1 R3: the component store plus what the editor owes a write. An edit marks the map's file dirty
    // for TerrainMapPersistence (R29), and the other terrains bound to the same file follow it: they
    // share the pool entry and the GPU image, so no pixels are copied (R41). A new map is file-backed,
    // named terrain_edit_* (R32). A null project is a store over memory alone (tests).
    class EditorTerrainStore final : public TerrainComponentStore{
    public:
        EditorTerrainStore(Project* project, uint32_t sceneId, Scene* scene, Entity entity, TerrainComponent& terrain);
        ~EditorTerrainStore() override;

        EditorTerrainStore(const EditorTerrainStore&) = delete;
        EditorTerrainStore& operator=(const EditorTerrainStore&) = delete;

        // While a stroke holds it the surface revision keeps its stroke rate (R23) and the map's file
        // waits (R29). The destructor ends what is still open.
        void beginStroke();
        void endStroke();

        // While set, the last fill leaves the hole map in place, all solid: edit_terrain removes it as
        // a structural step of its own, which its undo can put back
        void setKeepsMaps(bool keeps) { keepsMaps = keeps; }
        bool canRemoveMap(terraincore::MapId map) const override { return !keepsMaps && TerrainComponentStore::canRemoveMap(map); }

        terraincore::WriteResult write(terraincore::MapId map, terraincore::TexelRect rect, std::span<const float> in) override;
        terraincore::WriteResult writeBytes(terraincore::MapId map, terraincore::TexelRect rect, std::span<const uint8_t> in) override;
        terraincore::WriteResult createMap(terraincore::MapId map, terraincore::MapFormat format, float fill) override;
        void takeDirty(std::vector<std::pair<terraincore::MapId, terraincore::TexelRect>>& out) override;

    private:
        struct Sharer{
            uint32_t sceneId = 0;
            Entity entity = NULL_ENTITY;
        };
        struct Sharing{
            terraincore::MapId map;
            std::vector<Sharer> others;
        };

        void afterEdit();
        const std::vector<Sharer>& sharersOf(terraincore::MapId map);

        Project* project;
        uint32_t sceneId;
        // held while stroking: a stroke's end reaches the state whatever became of the component
        std::shared_ptr<TerrainEditState> strokeState;
        std::vector<std::pair<terraincore::MapId, terraincore::TexelRect>> edits;
        std::vector<Sharing> sharing;
        bool keepsMaps = false;
    };

}
