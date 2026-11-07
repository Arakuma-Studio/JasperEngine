// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainIssues.h"

#include "component/TerrainComponent.h"
#include "object/TerrainEdit.h"
#include "physics/JoltShapeBuilder.h"
#include "texture/TextureData.h"

#include <algorithm>
#include <cstdint>

using namespace jasperengine;

namespace {

    void add(std::vector<editor::TerrainIssue>& issues, const char* code, editor::TerrainIssueSeverity severity, const std::string& message,
             editor::TerrainIssueFix fix = editor::TerrainIssueFix::None, const char* fixLabel = ""){
        issues.push_back(editor::TerrainIssue{code, severity, message, fix, fixLabel});
    }

}

const char* editor::TerrainIssues::severityName(TerrainIssueSeverity severity){
    switch (severity){
        case TerrainIssueSeverity::Error: return "error";
        case TerrainIssueSeverity::Warning: return "warning";
        case TerrainIssueSeverity::Info: break;
    }
    return "info";
}

std::vector<editor::TerrainIssue> editor::TerrainIssues::check(TerrainComponent& terrain, size_t failedWrites){
    std::vector<TerrainIssue> issues;
    using S = TerrainIssueSeverity;

    if (terrain.terrainSize <= 0.0f){
        add(issues, "size_zero", S::Error, "Terrain Size is 0: the terrain has no extent.");
    }
    if (terrain.maxHeight == 0.0f){
        add(issues, "max_height_zero", S::Error, "Terrain Height is 0: every height is flat.");
    }
    if (failedWrites > 0){
        add(issues, "map_write_failed", S::Error, std::to_string(failedWrites) + " terrain map file(s) could not be written; the scene stays modified.",
            TerrainIssueFix::RetryWrite, "Retry Write");
    }

    if (terrain.heightMap.empty()){
        add(issues, "no_height_map", S::Error, "The terrain has no height map.", TerrainIssueFix::CreateTerrain, "Create Terrain...");
        return issues;
    }
    if (!terrain.heightMap.hasData() || !terrain.heightMap.getData().getData()){
        const TextureLoadResult result = terrain.heightMap.load();
        if (result.state == ResourceLoadState::Loading){
            add(issues, "height_map_loading", S::Info, "The height map is loading.");
        } else {
            add(issues, "map_load_failed", S::Error, "The height map \"" + terrain.heightMap.getPath() + "\" could not be loaded.",
                TerrainIssueFix::RetryLoad, "Retry");
        }
        return issues;
    }

    TextureData& data = terrain.heightMap.getData();
    const int bytes = TextureData::getBytesPerChannel(data.getColorFormat());
    if (bytes < 2){
        add(issues, "height_map_8bit", S::Warning, "The height map is 8-bit: heights move in 256 visible steps.",
            TerrainIssueFix::ConvertTo16Bit, "Convert to 16-bit");
    }

    // clipping: texels at the rails
    const int width = data.getWidth();
    const int height = data.getHeight();
    const int channels = std::max(1, data.getChannels());
    const unsigned char* pixels = static_cast<const unsigned char*>(data.getData());
    int64_t low = 0, high = 0;
    const int64_t texels = static_cast<int64_t>(width) * height;
    const uint32_t top = bytes >= 2 ? 65535u : 255u;
    for (int64_t i = 0; i < texels; i++){
        const unsigned char* at = pixels + i * channels * bytes;
        const uint32_t value = bytes >= 2 ? (at[0] | (at[1] << 8)) : at[0];
        if (value == 0) low++;
        else if (value == top) high++;
    }
    if (texels > 0 && static_cast<double>(low) / static_cast<double>(texels) >= CLIPPED_SHARE){
        add(issues, "height_clipped_low", S::Warning, std::to_string(low) + " height texels sit at 0, clipped.",
            TerrainIssueFix::RaiseTerrainHeight, "Raise Terrain Height");
    }
    if (texels > 0 && static_cast<double>(high) / static_cast<double>(texels) >= CLIPPED_SHARE){
        add(issues, "height_clipped_high", S::Warning, std::to_string(high) + " height texels sit at Terrain Height, clipped.",
            TerrainIssueFix::RaiseTerrainHeight, "Raise Terrain Height");
    }

    if (!terrain.heightMapLoaded || terrain.needUpdateTerrain){
        add(issues, "mesh_unbuilt", S::Info, "The terrain's mesh is being built from the height map.");
    }
    if (std::min(width, height) > static_cast<int>(JoltShapeBuilder::MAX_HEIGHTFIELD_SAMPLES)){
        add(issues, "collision_resampled", S::Info, "The collider samples " + std::to_string(JoltShapeBuilder::MAX_HEIGHTFIELD_SAMPLES) +
            " per side, fewer than the height map's " + std::to_string(std::min(width, height)) + ".");
    }
    if (terrain.allowRuntimeEdits){
        const std::string refusal = terrainRuntimeEditRefusal(terrain);
        if (!refusal.empty()){
            add(issues, "runtime_edit_map_too_large", S::Error, "Allow Runtime Edits is on, but " + refusal + ".");
        }
    }

    for (size_t i = 0; i < terrain.surfaceLayers.size(); i++){
        if (terrain.surfaceLayers[i].colorTexture.empty()){
            add(issues, "layer_without_texture", S::Warning, "Layer " + std::to_string(i) + " has no texture: its paint shows the base material.");
        }
    }
    for (size_t i = 0; i < terrain.blendMaps.size(); i++){
        Texture& blend = terrain.blendMaps[i];
        if (!blend.empty() && blend.hasData() && blend.getData().getData() && blend.getData().getChannels() < 3){
            add(issues, "blend_map_format", S::Warning, "Blend map " + std::to_string(i) + " has fewer than three channels: some layers cannot be painted.");
        }
    }
    return issues;
}
