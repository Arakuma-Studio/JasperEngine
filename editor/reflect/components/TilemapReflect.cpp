// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Grid tilemaps (docs/specs/S50-grid-tilemap.md §6.3): the Grid, Tilemap Layer and Tilemap
// Collider 2D components. Field order is the YAML key order. A layer's cells are one field, its
// "chunks": a map of "<cx>,<cy>" to the chunk's base64 runs (R86), in the save order (cy, cx).

#include "reflect/Components.h"

#include "Factory.h"
#include "Log.h"
#include "component/GridComponent.h"
#include "component/TilemapCollider2DComponent.h"
#include "component/TilemapLayerComponent.h"

#include <cstdio>
#include <string>

namespace jasperengine::editor::reflect {

namespace enums {

    namespace {
        constexpr int tileValue(TileGridLayout v) { return static_cast<int>(v); }
        constexpr int tileValue(TileGridPlane v) { return static_cast<int>(v); }
        constexpr int tileValue(StaggerAxis v) { return static_cast<int>(v); }
        constexpr int tileValue(StaggerIndex v) { return static_cast<int>(v); }
        constexpr int tileValue(TilemapRenderMode v) { return static_cast<int>(v); }
        constexpr int tileValue(TilemapGpuResidency v) { return static_cast<int>(v); }
        constexpr int tileValue(TileColliderSource v) { return static_cast<int>(v); }
        constexpr int tileValue(TileColliderGeometry v) { return static_cast<int>(v); }

        constexpr EnumItem kTileGridLayoutItems[] = {
            {tileValue(TileGridLayout::Rectangle), "rectangle", "Rectangle", "TileGridLayout::Rectangle"},
            {tileValue(TileGridLayout::Isometric), "isometric", "Isometric", "TileGridLayout::Isometric"},
            {tileValue(TileGridLayout::IsometricStaggered), "isometric_staggered", "Isometric Staggered", "TileGridLayout::IsometricStaggered"},
            {tileValue(TileGridLayout::Hexagon), "hexagon", "Hexagon", "TileGridLayout::Hexagon"},
        };
        constexpr EnumItem kTileGridPlaneItems[] = {
            {tileValue(TileGridPlane::XY), "xy", "XY", "TileGridPlane::XY"},
            {tileValue(TileGridPlane::XZ), "xz", "XZ", "TileGridPlane::XZ"},
        };
        constexpr EnumItem kStaggerAxisItems[] = {
            {tileValue(StaggerAxis::X), "x", "X", "StaggerAxis::X"},
            {tileValue(StaggerAxis::Y), "y", "Y", "StaggerAxis::Y"},
        };
        constexpr EnumItem kStaggerIndexItems[] = {
            {tileValue(StaggerIndex::Odd), "odd", "Odd", "StaggerIndex::Odd"},
            {tileValue(StaggerIndex::Even), "even", "Even", "StaggerIndex::Even"},
        };
        constexpr EnumItem kTilemapRenderModeItems[] = {
            {tileValue(TilemapRenderMode::Chunk), "chunk", "Chunk", "TilemapRenderMode::Chunk"},
            {tileValue(TilemapRenderMode::Individual), "individual", "Individual", "TilemapRenderMode::Individual"},
        };
        constexpr EnumItem kTilemapGpuResidencyItems[] = {
            {tileValue(TilemapGpuResidency::Visible), "visible", "Visible", "TilemapGpuResidency::Visible"},
            {tileValue(TilemapGpuResidency::Always), "always", "Always", "TilemapGpuResidency::Always"},
        };
        constexpr EnumItem kTileColliderSourceItems[] = {
            {tileValue(TileColliderSource::Tiles), "tiles", "Tiles", "TileColliderSource::Tiles"},
            {tileValue(TileColliderSource::Terrain), "terrain", "Terrain", "TileColliderSource::Terrain"},
        };
        constexpr EnumItem kTileColliderGeometryItems[] = {
            {tileValue(TileColliderGeometry::Outlines), "outlines", "Outlines", "TileColliderGeometry::Outlines"},
            {tileValue(TileColliderGeometry::Polygons), "polygons", "Polygons", "TileColliderGeometry::Polygons"},
        };
    }

    const EnumDesc kTileGridLayout{"TileGridLayout", kTileGridLayoutItems, tileValue(TileGridLayout::Rectangle)};
    const EnumDesc kTileGridPlane{"TileGridPlane", kTileGridPlaneItems, tileValue(TileGridPlane::XY)};
    const EnumDesc kStaggerAxis{"StaggerAxis", kStaggerAxisItems, tileValue(StaggerAxis::Y)};
    const EnumDesc kStaggerIndex{"StaggerIndex", kStaggerIndexItems, tileValue(StaggerIndex::Odd)};
    const EnumDesc kTilemapRenderMode{"TilemapRenderMode", kTilemapRenderModeItems, tileValue(TilemapRenderMode::Chunk)};
    const EnumDesc kTilemapGpuResidency{"TilemapGpuResidency", kTilemapGpuResidencyItems, tileValue(TilemapGpuResidency::Visible)};
    const EnumDesc kTileColliderSource{"TileColliderSource", kTileColliderSourceItems, tileValue(TileColliderSource::Tiles)};
    const EnumDesc kTileColliderGeometry{"TileColliderGeometry", kTileColliderGeometryItems, tileValue(TileColliderGeometry::Outlines)};

}

namespace {

    constexpr uint64_t kTile = UpdateFlags_TileLayer;
    constexpr uint32_t kRuntimeCount = Transient | ReadOnly | NoMcpWrite | NoDefault | NoCodegen;

    YAML::Node cellsToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
        const TileCells& cells = static_cast<const TileCellsRef*>(value)->read();
        YAML::Node node(YAML::NodeType::Map);
        for (const ChunkCoord& coord : cells.sortedChunks()) {
            char key[32];
            std::snprintf(key, sizeof(key), "%d,%d", coord.x, coord.y);
            node[key] = cells.encodeChunk(coord);
        }
        return node;
    }

    bool cellsFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string& reason) {
        TileCellsRef decoded;
        if (!node.IsMap()) {
            reason = "chunks is not a map of \"<cx>,<cy>\" to chunk strings";
            return false;
        }
        bool reservedBits = false;
        for (auto it = node.begin(); it != node.end(); ++it) {
            const std::string key = it->first.as<std::string>();
            ChunkCoord coord;
            char tail = 0;
            if (std::sscanf(key.c_str(), "%d,%d%c", &coord.x, &coord.y, &tail) != 2) {
                reason = "chunk key \"" + key + "\" is not \"<cx>,<cy>\"";
                return false;
            }
            std::string error;
            if (!decoded.edit().decodeChunk(coord, it->second.as<std::string>(), error)) {
                reason = "chunk \"" + key + "\": " + error;
                return false;
            }
            if (const TileChunk* chunk = decoded.read().chunk(coord)) {
                for (uint32_t cell : chunk->cells)
                    reservedBits = reservedBits || (cell & TILE_RESERVED_MASK) != 0;
            }
        }
        if (reservedBits)
            Log::warn("A Tilemap Layer has cells with reserved bits set: they are kept as they are");
        *static_cast<TileCellsRef*>(value) = std::move(decoded);
        return true;
    }

    bool cellsEqual(const void* a, const void* b, float) {
        return *static_cast<const TileCellsRef*>(a) == *static_cast<const TileCellsRef*>(b);
    }

    bool cellsSkip(const FieldDesc&, const void* value) {
        return static_cast<const TileCellsRef*>(value)->read().empty();
    }

    constexpr TypeOps kCellsOps{"TileCells", Kind::Custom, PropertyType::Custom, sizeof(TileCellsRef),
        &detail::construct<TileCellsRef>, &detail::destroy<TileCellsRef>, &detail::copy<TileCellsRef>, &cellsEqual,
        &cellsToYaml, &cellsFromYaml, nullptr, nullptr, &cellsSkip, false, nullptr, &detail::customSchema};

    constexpr FieldDesc kGridFields[] = {
        JE_FIELD(GridComponent, layout, .enumDesc = &enums::kTileGridLayout, .updateFlags = kTile),
        JE_FIELD(GridComponent, cellSize, .updateFlags = kTile, .label = "Cell Size"),
        JE_FIELD(GridComponent, cellGap, .updateFlags = kTile, .label = "Cell Gap"),
        JE_FIELD(GridComponent, plane, .enumDesc = &enums::kTileGridPlane, .updateFlags = kTile),
        JE_FIELD(GridComponent, staggerAxis, .enumDesc = &enums::kStaggerAxis, .updateFlags = kTile, .label = "Stagger Axis"),
        JE_FIELD(GridComponent, staggerIndex, .enumDesc = &enums::kStaggerIndex, .updateFlags = kTile, .label = "Stagger Index"),
        JE_FIELD(GridComponent, hexSideLength, .flags = OmitIfDefault | CodegenIfChanged, .updateFlags = kTile, .min = 0.0f, .label = "Hex Side Length"),
        JE_FIELD(GridComponent, importSource, .flags = OmitIfDefault | CodegenIfChanged, .asset = AssetKind::Any, .label = "Import Source"),
        JE_FIELD(GridComponent, linked, .flags = OmitIfDefault | CodegenIfChanged),
    };

    constexpr FieldDesc kTilemapLayerFields[] = {
        JE_FIELD(TilemapLayerComponent, version, .flags = ReadOnly | NoMcpWrite),
        JE_FIELD(TilemapLayerComponent, enabled, .updateFlags = kTile),
        JE_FIELD(TilemapLayerComponent, tileset, .updateFlags = kTile, .asset = AssetKind::Tileset),
        JE_FIELD(TilemapLayerComponent, tint, .updateFlags = kTile),
        JE_FIELD(TilemapLayerComponent, mode, .enumDesc = &enums::kTilemapRenderMode, .updateFlags = kTile),
        JE_FIELD(TilemapLayerComponent, chunkSize, .updateFlags = kTile, .min = 8.0f, .max = 64.0f, .label = "Chunk Size"),
        JE_FIELD(TilemapLayerComponent, tileAnchor, .flags = OmitIfDefault | CodegenIfChanged, .updateFlags = kTile, .label = "Tile Anchor"),
        JE_FIELD(TilemapLayerComponent, animationSpeed, .flags = OmitIfDefault | CodegenIfChanged, .min = 0.0f, .label = "Animation Speed"),
        JE_FIELD(TilemapLayerComponent, dualGrid, .updateFlags = kTile, .label = "Dual Grid"),
        JE_FIELD(TilemapLayerComponent, seed, .updateFlags = kTile),
        JE_FIELD(TilemapLayerComponent, gpuResidency, .enumDesc = &enums::kTilemapGpuResidency, .flags = OmitIfDefault | CodegenIfChanged, .label = "GPU Residency"),
        JE_FIELD_NAMED(TilemapLayerComponent, cells, "chunks", .ops = &kCellsOps),
        JE_FIELD(TilemapLayerComponent, cellCount, .flags = kRuntimeCount, .label = "Cells"),
        JE_FIELD(TilemapLayerComponent, chunkCount, .flags = kRuntimeCount, .label = "Chunks"),
    };

    constexpr FieldDesc kTilemapColliderFields[] = {
        JE_FIELD(TilemapCollider2DComponent, enabled, .updateFlags = kTile),
        JE_FIELD(TilemapCollider2DComponent, source, .enumDesc = &enums::kTileColliderSource, .updateFlags = kTile),
        JE_FIELD(TilemapCollider2DComponent, solidTerrains, .flags = OmitIfDefault | CodegenIfChanged | ListWhole, .updateFlags = kTile, .label = "Solid Terrains"),
        JE_FIELD(TilemapCollider2DComponent, geometry, .enumDesc = &enums::kTileColliderGeometry, .updateFlags = kTile),
        JE_FIELD(TilemapCollider2DComponent, merge, .updateFlags = kTile),
        JE_FIELD(TilemapCollider2DComponent, offset, .flags = OmitIfDefault | CodegenIfChanged, .updateFlags = kTile),
        JE_FIELD(TilemapCollider2DComponent, friction, .updateFlags = kTile, .min = 0.0f),
        JE_FIELD(TilemapCollider2DComponent, restitution, .updateFlags = kTile, .min = 0.0f),
        // a .physicsmaterial path as Collider2D's and Rigidbody2D's: a string with .asset needs a "<name>Guid" sibling
        JE_FIELD(TilemapCollider2DComponent, material, .flags = OmitIfDefault | CodegenIfChanged, .updateFlags = kTile),
        JE_FIELD(TilemapCollider2DComponent, categoryBits, .flags = OmitIfDefault | CodegenIfChanged, .updateFlags = kTile),
        JE_FIELD(TilemapCollider2DComponent, maskBits, .flags = OmitIfDefault | CodegenIfChanged, .updateFlags = kTile),
        JE_FIELD(TilemapCollider2DComponent, groupIndex, .flags = OmitIfDefault | CodegenIfChanged, .updateFlags = kTile),
        JE_FIELD(TilemapCollider2DComponent, contactEvents, .flags = OmitIfDefault | CodegenIfChanged, .updateFlags = kTile),
        JE_FIELD(TilemapCollider2DComponent, sensorEvents, .flags = OmitIfDefault | CodegenIfChanged, .updateFlags = kTile),
        JE_FIELD(TilemapCollider2DComponent, enableHitEvents, .flags = OmitIfDefault | CodegenIfChanged, .updateFlags = kTile),
        JE_FIELD(TilemapCollider2DComponent, generatedChains, .flags = kRuntimeCount, .label = "Chains"),
        JE_FIELD(TilemapCollider2DComponent, generatedPolygons, .flags = kRuntimeCount, .label = "Polygons"),
    };

    // a chunk string per statement, under MSVC's literal limit (R87: 16 KB a literal)
    void tilemapLayerCodegen(const void* comp, const CodegenContext& ctx, std::string& out) {
        const TileCells& cells = static_cast<const TilemapLayerComponent*>(comp)->cells.read();
        const std::string ind = Factory::indentation(ctx.indentSpaces);
        for (const ChunkCoord& coord : cells.sortedChunks()) {
            const std::string text = cells.encodeChunk(coord);
            out += ind + "{\n" + ind + "    std::string chunkError;\n" + ind + "    " + ctx.variable + ".cells.edit().decodeChunk(ChunkCoord{" +
                   std::to_string(coord.x) + ", " + std::to_string(coord.y) + "}, ";
            for (size_t i = 0; i < text.size(); i += 16000)
                out += "\n" + ind + "        \"" + text.substr(i, 16000) + "\"";
            out += ", chunkError);\n" + ind + "}\n";
        }
    }

    // the solid terrains' names: a list of strings the field tables do not export
    void tilemapColliderCodegen(const void* comp, const CodegenContext& ctx, std::string& out) {
        const TilemapCollider2DComponent& collider = *static_cast<const TilemapCollider2DComponent*>(comp);
        if (collider.solidTerrains.empty()) return;
        std::string list;
        for (const std::string& name : collider.solidTerrains) list += (list.empty() ? "" : ", ") + Factory::formatString(name);
        out += Factory::indentation(ctx.indentSpaces) + ctx.variable + ".solidTerrains = {" + list + "};\n";
    }

    constexpr const char* kTilemapLayerCodegenOrder[] = {"tileset", "tint", "mode", "chunkSize", "tileAnchor", "animationSpeed", "dualGrid", "seed", "gpuResidency", "enabled"};

}

const ComponentDesc kGridDesc{
    .type = ComponentType::GridComponent, .cppName = "GridComponent", .key = "grid",
    .displayName = "Grid", .icon = ICON_FA_TABLE_CELLS,
    .body = JE_STRUCT_DESC(GridComponent, "GridComponent", kGridFields),
    .hasEnabled = false, .structural = false, JE_COMPONENT_ACCESS(GridComponent),
    .codegenVar = "grid",
};

const ComponentDesc kTilemapLayerDesc{
    .type = ComponentType::TilemapLayerComponent, .cppName = "TilemapLayerComponent", .key = "tilemaplayer",
    .displayName = "Tilemap Layer", .icon = ICON_FA_BORDER_ALL,
    .body = JE_STRUCT_DESC(TilemapLayerComponent, "TilemapLayerComponent", kTilemapLayerFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(TilemapLayerComponent),
    .codegenVar = "tilemapLayer", .codegenOrder = kTilemapLayerCodegenOrder, .codegenExtra = &tilemapLayerCodegen,
};

const ComponentDesc kTilemapCollider2DDesc{
    .type = ComponentType::TilemapCollider2DComponent, .cppName = "TilemapCollider2DComponent", .key = "tilemapcollider2d",
    .displayName = "Tilemap Collider 2D", .icon = ICON_FA_BORDER_ALL,
    .body = JE_STRUCT_DESC(TilemapCollider2DComponent, "TilemapCollider2DComponent", kTilemapColliderFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(TilemapCollider2DComponent),
    .codegenVar = "tilemapCollider2D", .codegenExtra = &tilemapColliderCodegen,
};

}
