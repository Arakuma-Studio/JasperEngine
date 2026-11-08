// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "ecs/Entity.h"
#include "terrain/OverlayMapStore.h"
#include "texture/TextureData.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Project;

    // One undo step of map edits (T1 R24-R26, DC7): per changed map, the 64x64 tiles that changed, each
    // the XOR of its before and after bytes, zstd compressed when that saves bytes, all-zero tiles left
    // out. Undo and redo are the same XOR, through the store, so an undo refits, publishes and persists
    // like any write. It applies only to the map it was cut from.
    class TerrainMapDeltaCmd: public Command{
    public:
        // Cut from the overlay's tiles. alreadyApplied: the first execute finds the map as it should
        // be (a stroke, which showed itself as it went); otherwise it applies the XOR. verb names the
        // step when it is not a sculpt or a paint ("Apply Spline to Terrain").
        // skipMaps: maps whose bytes another step of the same group carries (a map the edit made,
        // whose structural step holds its final pixels, so its redo needs no map to exist first)
        TerrainMapDeltaCmd(Project* project, uint32_t sceneId, Entity entity, Entity nameEntity, const terrain::OverlayMapStore& overlay,
                           bool alreadyApplied, std::string verb = {}, const std::vector<terrain::MapId>& skipMaps = {});

        // What the store created on write, or a registered op's non-map data, in the same step (R86, R89)
        void addUndoHook(std::shared_ptr<terrain::TerrainStoreUndoHook> hook);
        void addLargeEdit(std::shared_ptr<terrain::TerrainLargeEdit> edit);

        // Nothing changed: not worth a step
        bool empty() const { return maps.empty() && undoHooks.empty() && largeEdits.empty(); }

        bool execute() override;
        void undo() override;

        std::string undoRefusal() const override;
        std::string redoRefusal() const override;
        std::vector<Touched> touchedPaths(bool undo) const override;

        bool affectsStructure() const override { return false; }
        bool isNoOp() const override { return empty(); }
        std::string describe() const override;
        const char* typeName() const override { return "TerrainMapDeltaCmd"; }
        size_t memoryBytes() const override;
        // The destructor frees bytes and strings; what a hook holds is not known here (S24 R8)
        bool releaseOffThread() const override { return undoHooks.empty() && largeEdits.empty(); }

        // Tests: how many tiles the step holds, and how many of them stayed raw
        size_t tileCount() const;
        size_t rawTileCount() const;

    private:
        // sizeof is what R26 calls 32 bytes per tile: its origin locates it, and its size says
        // whether it is compressed
        struct Tile{
            int x = 0;
            int y = 0;
            std::vector<uint8_t> bytes;
        };

        struct MapDelta{
            terrain::MapId map;
            std::string path;
            std::string id;
            terrain::TexelRect bounds;
            int channels = 0;
            ColorFormat colorFormat = ColorFormat::RGBA;
            uint32_t generation = 0;
            std::vector<Tile> tiles;
        };

        std::string refusal() const;
        void applyXor(bool makesTiles);

        Project* project;
        uint32_t sceneId;
        Entity entity;
        Entity nameEntity;
        std::string verb;
        std::vector<MapDelta> maps;
        std::vector<std::shared_ptr<terrain::TerrainStoreUndoHook>> undoHooks;
        std::vector<std::shared_ptr<terrain::TerrainLargeEdit>> largeEdits;
        bool applied;
        // executed once: later executes are redos
        bool replayed = false;
        bool wasModified = false;
    };

}
