// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Terrain World component (docs/specs/T4-terrain-worlds.md R1, R15, R41) on the generic codec.
// Values at their defaults are not written, so a world in a scene is a few lines and a scene without
// one is unchanged (R54). The lattice and the maps belong to the .terrainworld asset.

#include "reflect/Components.h"

#include "component/TerrainWorldComponent.h"

namespace jasperengine::editor::reflect {

namespace {

    constexpr uint32_t kOmit = OmitIfDefault | EmptyIsDefault | CodegenIfChanged;
    constexpr uint32_t kOmitFloat = OmitIfDefault | OmitIfNonFinite | EmptyIsDefault | CodegenIfChanged;

    constexpr FieldDesc kTerrainWorldFields[] = {
        JE_FIELD(TerrainWorldComponent, enabled, .flags = kOmit),
        JE_FIELD(TerrainWorldComponent, worldData, .flags = kOmit, .asset = AssetKind::TerrainWorld, .label = "World Data",
                 .tooltip = "The .terrainworld folder: its lattice, maps and tiles."),
        JE_FIELD(TerrainWorldComponent, useCameraFocus, .flags = kOmit, .label = "Use Camera Focus",
                 .tooltip = "The active camera streams the world (in edit mode, the Scene view's)."),
        JE_FIELD(TerrainWorldComponent, focus, .ops = &ops::Entity, .flags = kOmit,
                 .tooltip = "One more focus that follows this GameObject; scripts add others."),
        JE_FIELD(TerrainWorldComponent, poolSize, .flags = kOmit | Clamped, .min = 0.0f, .max = 512.0f, .label = "Pool Size",
                 .tooltip = "Tiles drawn from their own pages at once. 0: the platform's (64 desktop, 20 mobile, 16 web)."),
        JE_FIELD(TerrainWorldComponent, lodDistance, .ops = &ops::FiniteFloat, .flags = kOmitFloat | Clamped, .min = 0.0f, .max = 8.0f,
                 .label = "LOD Distance", .tooltip = "How far the near band reaches, as a multiplier. 0: the platform's."),
        JE_FIELD(TerrainWorldComponent, uploadBudget, .ops = &ops::FiniteFloat, .flags = kOmitFloat | Clamped, .min = 0.0f, .max = 256.0f,
                 .label = "Upload Budget", .tooltip = "MiB of tile pages uploaded a frame. 0: the platform's."),
        JE_FIELD(TerrainWorldComponent, collisionRadius, .ops = &ops::FiniteFloat, .flags = kOmitFloat | Clamped, .unit = Unit::Meters, .min = 0.0f,
                 .max = 4096.0f, .label = "Collision Radius", .tooltip = "Fine collision round each focus. 0: the platform's."),
        JE_FIELD(TerrainWorldComponent, collisionChunkQuads, .flags = kOmit | Clamped, .min = 32.0f, .max = 1024.0f, .label = "Collision Chunk",
                 .tooltip = "Quads a side of a paged collider: a power of two from 32."),
        JE_FIELD(TerrainWorldComponent, autoAddTiles, .flags = kOmit, .label = "Auto Add Tiles",
                 .tooltip = "Brushes, edit_terrain and generation make the tiles they write over."),
        JE_FIELD(TerrainWorldComponent, overviewCastsShadows, .flags = kOmit, .label = "Overview Casts Shadows",
                 .tooltip = "The far overview casts shadows too."),
    };

}

const ComponentDesc kTerrainWorldDesc{
    .type = ComponentType::TerrainWorldComponent, .cppName = "TerrainWorldComponent", .key = "terrainworld",
    .displayName = "Terrain World", .icon = ICON_FA_MAP,
    .body = JE_STRUCT_DESC(TerrainWorldComponent, "TerrainWorldComponent", kTerrainWorldFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(TerrainWorldComponent),
    .codegenVar = "terrainWorld",
};

}
