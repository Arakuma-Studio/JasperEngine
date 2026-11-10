// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "component/TerrainComponent.h"

#include <string>
#include <vector>

// The valid values of a Terrain detail layer (Unity's Details; TerrainComponent::foliageLayers),
// shared by edit_terrain_details, set_properties on foliageLayers and the scene decoder
namespace jasperengine::editor::terraindetail {

    struct DetailRange {
        const char* key;                        // the tool's snake_case field
        float TerrainFoliageLayer::* field;
        float min;
        float max;
        const char* meaning;                    // why the range, for the refusal
    };

    // density, scales, rotation and alignment, slopes, heights, draw distance
    const std::vector<DetailRange>& detailRanges();

    // a model file the import pipeline loads (Util::isModelFile: .obj, .gltf, .glb, .fbx)
    bool isDetailMeshPath(const std::string& path);

    // The first violation ("" when the layer is valid): a range, a min above its max, the mesh type
    std::string checkDetailLayer(const TerrainFoliageLayer& layer);

    // Clamps every range and swaps inverted pairs; returns what it changed ("density -5 -> 0")
    std::vector<std::string> clampDetailLayer(TerrainFoliageLayer& layer);

}
