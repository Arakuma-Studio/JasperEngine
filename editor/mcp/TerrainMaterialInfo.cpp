// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainMaterialInfo.h"
#include "Engine.h"

#include "component/LightComponent.h"
#include "object/TerrainEdit.h"
#include "physics/JoltShapeBuilder.h"
#include "subsystem/TerrainRenderInfo.h"
#include "terrain/TerrainHoles.h"
#include "terrain/TerrainLayers.h"
#include "texture/TextureData.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor::mcp;

namespace {

    const char* qualitySourceName(terrain::TerrainQualitySource source) {
        switch (source) {
            case terrain::TerrainQualitySource::EDITOR_PREVIEW: return "editor_preview";
            case terrain::TerrainQualitySource::GLOBAL_OVERRIDE: return "global_override";
            case terrain::TerrainQualitySource::COMPONENT: return "component";
            default: return "project";
        }
    }

    Json fetchesJson(const terrain::MaterialCost& cost) {
        return Json{{"flat", cost.flat}, {"slope", cost.slope}, {"worst", cost.worst}};
    }

    // the blend map texels of a layer's weight at a local point, nearest: enough for the agent's answer,
    // and the same mapping the shader reads (terrain::terrainLayerChannel)
    void paintedWeights(TerrainComponent& component, float localX, float localZ, std::vector<float>& weights) {
        const int layers = std::min(static_cast<int>(component.surfaceLayers.size()), terrain::TERRAIN_MAX_LAYERS);
        weights.assign(static_cast<size_t>(layers), 0.0f);
        const float half = component.terrainSize * 0.5f;
        for (int layer = 0; layer < layers; layer++) {
            const terrain::LayerChannel channel = terrain::terrainLayerChannel(layer);
            if (channel.map >= static_cast<int>(component.blendMaps.size())) continue;
            Texture& map = component.blendMaps[static_cast<size_t>(channel.map)];
            if (map.empty() || map.isFramebuffer() || !map.hasData() || !map.getData().getData()) continue;
            TextureData& data = map.getData();
            const int x = std::clamp(static_cast<int>((localX + half) / component.terrainSize * static_cast<float>(data.getWidth())), 0, data.getWidth() - 1);
            const int y = std::clamp(static_cast<int>((localZ + half) / component.terrainSize * static_cast<float>(data.getHeight())), 0, data.getHeight() - 1);
            const int channels = data.getChannels();
            if (channel.channel >= channels) {
                // no alpha: opaque, which weights an extended layer 0
                continue;
            }
            const float stored = static_cast<const uint8_t*>(data.getData())[(static_cast<size_t>(y) * data.getWidth() + x) * channels + channel.channel] / 255.0f;
            weights[static_cast<size_t>(layer)] = channel.inverted ? 1.0f - stored : stored;
        }
    }

}

terrain::TerrainMaterialInputs jasperengine::editor::mcp::terrainMaterialInputs(const TerrainComponent& component, bool postColor) {
    terrain::TerrainMaterialInputs inputs;
    inputs.layerCount = std::min(static_cast<int>(component.surfaceLayers.size()), terrain::TERRAIN_MAX_LAYERS);
    for (int i = 0; i < inputs.layerCount; i++) {
        const TerrainSurfaceLayer& layer = component.surfaceLayers[static_cast<size_t>(i)];
        inputs.layers[i].pbr = layer.pbr;
        inputs.layers[i].normalMap = !layer.normalTexture.empty();
        inputs.layers[i].surfaceMap = !layer.occlusionTexture.empty() || !layer.roughnessTexture.empty() ||
                                      !layer.metallicTexture.empty() || !layer.heightTexture.empty();
        inputs.layers[i].flatOnly = layer.cliffProjection == TerrainCliffProjection::FLAT_ONLY;
        inputs.layers[i].hex = layer.hexTiling;
        inputs.layers[i].stochastic = layer.stochasticTiling;
    }
    inputs.limitLayersPerPixel = component.limitLayersPerPixel;
    inputs.holes = !component.holeMap.empty();
    inputs.macroNormal = component.macroNormal;
    inputs.resample = component.resampleEnabled;
    inputs.postColor = postColor;
    inputs.lightingSlots = postColor ? 8 : 7;
    return inputs;
}

terrain::TerrainQualityResolved jasperengine::editor::mcp::terrainEffectiveTier(const TerrainComponent& component) {
    terrain::TerrainQualityRequest request;
    terrain::TerrainQuality forced;
    if (terrain::TerrainQualitySettings::getEditorPreview(forced)) {
        request.hasPreview = true;
        request.preview = forced;
    } else if (terrain::TerrainQualitySettings::getGlobalOverride(forced)) {
        request.hasOverride = true;
        request.globalOverride = forced;
    }
    const terrain::TerrainVariantInputs variant = getTerrainVariantInputs(component);
    request.hasComponent = variant.hasComponentQuality;
    request.component = variant.componentQuality;
    request.project = terrain::TerrainQualitySettings::getPlatformTier(static_cast<int>(Engine::getPlatform()));
    request.shippedMask = terrain::TerrainQualitySettings::getShippedMask();
    return terrain::resolveTerrainQuality(request);
}

Json jasperengine::editor::mcp::terrainMaterialJson(Scene* scene, Entity entity) {
    TerrainComponent& component = scene->getComponent<TerrainComponent>(entity);
    const terrain::TerrainQualityResolved resolved = terrainEffectiveTier(component);
    const terrain::TerrainMaterialInputs inputs = terrainMaterialInputs(component, true);
    const terrain::MaterialCost cost = terrain::materialCost(inputs, resolved.quality);

    Json byTier = Json::object();
    for (int tier = 0; tier < terrain::TERRAIN_QUALITY_COUNT; tier++) {
        const terrain::TerrainQuality quality = static_cast<terrain::TerrainQuality>(tier);
        byTier[terrain::terrainQualityName(quality)] = fetchesJson(terrain::materialCost(inputs, quality));
    }
    Json fetches = fetchesJson(cost);
    fetches["by_tier"] = byTier;

    Json detail = nullptr;
    GpuTextureFormat format = GpuTextureFormat::RGBA8;
    int slices = 0;
    uint64_t bytes = 0;
    if (getTerrainDetailArrayInfo(scene, entity, format, slices, bytes)) {
        detail = Json{{"format", gpuFormatInfo(format).name}, {"slices", slices}, {"bytes", bytes}};
    }

    const terrain::TerrainTierInfo& tier = terrain::terrainTierInfo(resolved.quality);
    Json features = Json::array();
    const auto feature = [&](bool on, const char* name) {
        if (on) features.push_back(name);
    };
    feature(component.macroMode == TerrainMacroMode::NOISE, "macro_noise");
    feature(component.macroMode == TerrainMacroMode::BASE_MAP, "macro_base_map");
    feature(component.resampleEnabled && tier.distanceResampling, "distance_resampling");
    feature(component.heightBlendSharpness > 0.0f, "height_blend");
    feature(component.macroNormal && tier.macroNormal, "macro_normal");
    feature(hasTerrainCliffLayers(component), "cliff_projection");
    feature(!component.holeMap.empty(), "holes");
    // R21, R14: what a feature took from the base material, so nobody wonders where it went
    if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity); mesh && mesh->numSubmeshes > 0) {
        feature(!component.holeMap.empty() && !mesh->submeshes[0].material.occlusionTexture.empty(), "base_occlusion_dropped");
        feature(component.macroNormal && tier.macroNormal && !mesh->submeshes[0].material.normalTexture.empty(), "base_normal_replaced");
    }

    return Json{{"quality", terrain::terrainQualityName(resolved.quality)},
                {"quality_source", qualitySourceName(resolved.source)},
                {"limit_layers_per_pixel", component.limitLayersPerPixel},
                {"layers", inputs.layerCount},
                {"blend_maps_read", cost.blendMapsRead},
                {"fetches", fetches},
                {"texture_slots", {{"used", cost.textureSlots}, {"limit", terrain::TERRAIN_TEXTURE_SLOT_LIMIT}}},
                {"detail_array", detail},
                {"features", features}};
}

Json jasperengine::editor::mcp::terrainHolesJson(TerrainComponent& component) {
    if (component.holeMap.empty()) return nullptr;
    const terrain::HoleMapView view = terrainSurfaceView(component).holes;
    const int64_t holes = terrain::countHoleTexels(view, 0, 0, view.width, view.height);
    const int64_t total = static_cast<int64_t>(view.width) * view.height;
    const int samples = component.heightMap.hasData() ? static_cast<int>(JoltShapeBuilder::defaultHeightFieldSamples(component)) : 0;
    return Json{{"map", component.holeMap.getPath(0)},
                {"resolution", {view.width, view.height}},
                {"hole_texels", holes},
                {"hole_fraction", total > 0 ? static_cast<double>(holes) / static_cast<double>(total) : 0.0},
                {"collision_cell", samples > 1 ? component.terrainSize / static_cast<float>(samples - 1) : component.terrainSize}};
}

void jasperengine::editor::mcp::addTerrainPointMaterial(Scene* scene, Entity entity, float localX, float localZ, bool full, Json& sample) {
    TerrainComponent& component = scene->getComponent<TerrainComponent>(entity);
    const terrain::SurfaceSample surface = querySurface(component, localX, localZ);
    sample["hole"] = surface.hole;
    if (!full) return;

    bool gap = false;
    bool excluded = false;
    if (!component.holeMap.empty() && surface.inside) {
        const terrain::HoleMapView view = terrainSurfaceView(component).holes;
        const int samples = static_cast<int>(JoltShapeBuilder::defaultHeightFieldSamples(component));
        std::vector<uint8_t> open;
        std::vector<uint8_t> gaps;
        std::vector<uint8_t> cells;
        terrain::openSquares(view, samples, 1.0f, 1.0f, open);
        terrain::noCollisionSamples(open, samples, gaps);
        terrain::navHoleCells(view, samples, cells);
        const int spans = samples - 1;
        const float u = (localX + component.terrainSize * 0.5f) / component.terrainSize * static_cast<float>(spans);
        const float v = (localZ + component.terrainSize * 0.5f) / component.terrainSize * static_cast<float>(spans);
        const int i = std::clamp(static_cast<int>(std::floor(u)), 0, spans - 1);
        const int j = std::clamp(static_cast<int>(std::floor(v)), 0, spans - 1);
        const auto missing = [&](int x, int y) { return gaps[static_cast<size_t>(y) * samples + x] != 0; };
        // Jolt splits a square along (x, y)-(x+1, y+1): the triangle under the point loses it if a corner of its own went
        const bool upper = v - static_cast<float>(j) > u - static_cast<float>(i);
        gap = missing(i, j) || missing(i + 1, j + 1) || (upper ? missing(i, j + 1) : missing(i + 1, j));
        excluded = cells[static_cast<size_t>(j) * spans + i] != 0;
    }
    sample["collision_gap"] = gap;
    sample["nav_excluded"] = excluded;

    std::vector<float> weights;
    paintedWeights(component, localX, localZ, weights);
    Json painted = Json::array();
    for (size_t layer = 0; layer < weights.size(); layer++) {
        if (weights[layer] > 0.0f) painted.push_back({{"layer", layer}, {"weight", weights[layer]}});
    }
    sample["layers_painted"] = painted;

    const terrain::TerrainQuality quality = terrainEffectiveTier(component).quality;
    const terrain::TerrainTierInfo& tier = terrain::terrainTierInfo(quality);
    const int keep = component.limitLayersPerPixel ? tier.limitedLayersPerPixel : tier.layersPerPixel;
    const terrain::LayerSelection selection = terrain::selectLayers(weights.data(), static_cast<int>(weights.size()), keep);
    Json drawn = Json::array();
    for (int i = 0; i < selection.count; i++) {
        if (selection.weight[i] > 0.0f) drawn.push_back({{"layer", selection.layer[i]}, {"weight", selection.weight[i]}});
    }
    sample["layers_drawn"] = drawn;

    // this point's reads: the blend maps, each drawn layer's maps, and what the variant adds
    const terrain::TerrainMaterialInputs inputs = terrainMaterialInputs(component, true);
    const terrain::MaterialCost cost = terrain::materialCost(inputs, quality);
    int fetches = cost.blendMapsRead + (inputs.holes ? 1 : 0) + (inputs.macroNormal && tier.macroNormal ? 1 : 0);
    for (int i = 0; i < selection.count; i++) {
        if (selection.weight[i] <= 0.0f) continue;
        const terrain::TerrainLayerCostInput& layer = inputs.layers[selection.layer[i]];
        if (quality == terrain::TerrainQuality::LOW) {
            fetches += 1 + (i == 0 && layer.pbr && layer.normalMap ? 1 : 0);
        } else {
            fetches += 1 + (layer.pbr && layer.normalMap ? 1 : 0) + (layer.pbr && layer.surfaceMap ? 1 : 0);
        }
    }
    sample["fetches"] = fetches;
}

Json jasperengine::editor::mcp::terrainCoverageJson(TerrainComponent& component, float minX, float minZ, float maxX, float maxZ) {
    const float half = component.terrainSize * 0.5f;
    Json out = Json::object();
    const terrain::HoleMapView view = terrainSurfaceView(component).holes;
    double holeFraction = 0.0;
    if (view.texels) {
        const int x0 = static_cast<int>(std::floor((minX + half) / component.terrainSize * static_cast<float>(view.width)));
        const int y0 = static_cast<int>(std::floor((minZ + half) / component.terrainSize * static_cast<float>(view.height)));
        const int x1 = static_cast<int>(std::ceil((maxX + half) / component.terrainSize * static_cast<float>(view.width)));
        const int y1 = static_cast<int>(std::ceil((maxZ + half) / component.terrainSize * static_cast<float>(view.height)));
        const int64_t area = static_cast<int64_t>(std::clamp(x1, 0, view.width) - std::clamp(x0, 0, view.width)) * (std::clamp(y1, 0, view.height) - std::clamp(y0, 0, view.height));
        if (area > 0) holeFraction = static_cast<double>(terrain::countHoleTexels(view, x0, y0, x1, y1)) / static_cast<double>(area);
    }
    out["hole_fraction"] = holeFraction;

    // the histogram of how many layers a point has painted, over a grid of the rectangle
    constexpr int GRID = 64;
    int histogram[terrain::TERRAIN_MAX_LAYERS + 1] = {};
    std::vector<float> weights;
    for (int j = 0; j < GRID; j++) {
        const float z = minZ + (maxZ - minZ) * (static_cast<float>(j) + 0.5f) / GRID;
        for (int i = 0; i < GRID; i++) {
            const float x = minX + (maxX - minX) * (static_cast<float>(i) + 0.5f) / GRID;
            paintedWeights(component, x, z, weights);
            int count = 0;
            for (float weight : weights) count += weight > 0.0f ? 1 : 0;
            histogram[count]++;
        }
    }
    Json layerCounts = Json::object();
    for (int count = 0; count <= terrain::TERRAIN_MAX_LAYERS; count++) {
        if (histogram[count] > 0) layerCounts[std::to_string(count)] = static_cast<double>(histogram[count]) / (GRID * GRID);
    }
    out["layer_count_histogram"] = layerCounts;
    return out;
}
