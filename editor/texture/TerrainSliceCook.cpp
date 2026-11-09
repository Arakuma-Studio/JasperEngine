// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainSliceCook.h"

#include "Project.h"
#include "component/TerrainComponent.h"
#include "component/VoxelVolumeComponent.h"
#include "subsystem/VoxelSystem.h"
#include "texture/TerrainDetailBuilder.h"
#include "texture/CookedTextures.h"
#include "util/PngWriter.h"

#include <algorithm>
#include <filesystem>
#include <span>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    // The fixed settings of a slice (R16): one format per family, so an array's slices can concatenate,
    // and at most the tier table's largest for that family
    void sliceSettings(TextureImportSettings& settings, terraincore::DetailSliceKind kind) {
        settings.type = kind == terraincore::DetailSliceKind::NORMAL ? TextureImportType::Normal : TextureImportType::Default;
        // data to the array: the shader decodes a colour itself (terrain_fs.glsl), and one sampler reads them all
        settings.srgb = false;
        settings.mipmaps = true;
        settings.shipSource = false;
        settings.maxSize = MAX_TERRAIN_DETAIL_SIZE;
        const auto family = [&](TextureFamily which, const char* format, int maxSize) {
            TexturePlatformSettings& platform = settings.platforms[static_cast<size_t>(which)];
            platform.override = true;
            platform.format = format;
            platform.maxSize = maxSize;
        };
        family(TextureFamily::Desktop, "BC7", 2048);
        family(TextureFamily::Mobile, "ASTC_6x6", 1024);
        family(TextureFamily::AndroidEtc2, "ETC2_RGBA8", 1024);
        family(TextureFamily::Web, "UASTC", 2048);
    }

}

bool editor::gatherTerrainSliceInputs(Project& project, std::map<std::string, TextureCookInputs>& inputs, std::set<std::string>& droppedSources, std::string& error) {
    if (!project.isCompressTerrainLayers()) return true;

    bool allScenesLoaded = true;
    const fs::path dir = project.getProjectInternalPath() / "cache" / "terrain-slices";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        error = "cannot create " + dir.generic_string() + ": " + ec.message();
        return false;
    }

    std::set<std::string> cookedLayerSources;
    // a source an array that is not cooked reads, which must ship
    std::set<std::string> rawSources;
    const auto sourcesOf = [](std::span<const TerrainSurfaceLayer> layers, std::set<std::string>& out) {
        for (const TerrainSurfaceLayer& layer : layers) {
            for (const Texture* texture : {&layer.colorTexture, &layer.normalTexture, &layer.roughnessTexture, &layer.metallicTexture, &layer.occlusionTexture, &layer.heightTexture}) {
                if (!texture->empty()) out.insert(CookedTextures::normalize(texture->getPath(0)));
            }
        }
    };
    // Each slice of one array, under its key: 1 cooked whole, 0 not (its sources ship), -1 error
    const auto cookArray = [&](std::span<const TerrainSurfaceLayer> layers, const terraincore::DetailPlan& plan) -> int {
        bool complete = plan.anySource;
        for (size_t s = 0; s < plan.sliceKeys.size() && complete; s++) {
            if (plan.sliceKeys[s].empty()) {
                // a colour slot with no map stays white on the raw path: this array is not cooked
                complete = false;
                break;
            }
            const std::string key = terraincore::cookedSliceIndexKey(plan.sliceKeys[s]);
            if (inputs.count(key)) continue;
            std::vector<uint8_t> rgba;
            int width = 0;
            int height = 0;
            if (!terraincore::buildDetailSliceTop(layers, plan, static_cast<int>(s), MAX_TERRAIN_DETAIL_SIZE, rgba, width, height)) {
                complete = false;
                break;
            }
            TextureCookInputs in;
            in.assetPath = key;
            in.absolute = dir / fs::path(key).filename();
            in.projectRelative = project.normalizeToProjectRelative(in.absolute).generic_string();
            if (!writePng(in.absolute, width, height, 4, rgba.data(), width * 4, io::Durability::Cache)) {
                error = "cannot write " + in.absolute.generic_string();
                return -1;
            }
            sliceSettings(in.settings, terraincore::detailSliceKind(plan, static_cast<int>(s)));
            in.hasSettings = true;
            in.inference.linear = true;
            inputs.emplace(key, std::move(in));
        }
        sourcesOf(layers, complete ? cookedLayerSources : rawSources);
        return complete ? 1 : 0;
    };
    for (const SceneProject& sceneProject : project.getScenes()) {
        if (!sceneProject.scene) {
            allScenesLoaded = false;
            continue;
        }
        // T6 R50: a Voxel Volume's array, slot by slot as it draws, keyed as a terrain's, so a linked slot shares its slices
        auto volumes = sceneProject.scene->getComponentArray<VoxelVolumeComponent>();
        for (size_t v = 0; v < volumes->size(); v++) {
            const std::vector<TerrainSurfaceLayer> layers = voxelSlotLayers(sceneProject.scene, volumes->getComponentFromIndex(v));
            terraincore::DetailPlanOptions options;
            options.wholeBlendMaps = false;
            if (cookArray(layers, terraincore::planDetailArray(layers, options)) < 0) return false;
        }
        auto terrains = sceneProject.scene->getComponentArray<TerrainComponent>();
        for (size_t t = 0; t < terrains->size(); t++) {
            const TerrainComponent& component = terrains->getComponentFromIndex(t);
            const std::span<const TerrainSurfaceLayer> layers(component.surfaceLayers.data(), std::min(component.surfaceLayers.size(), static_cast<size_t>(MAX_TERRAIN_LAYERS)));
            if (cookArray(layers, terraincore::planDetailArray(component)) < 0) return false;
        }
    }
    for (const std::string& source : rawSources) cookedLayerSources.erase(source);

    // R16: a source goes when every reference to it reads it as a terrain layer; with a scene unread a
    // reference may be anywhere, so nothing goes
    if (allScenesLoaded) {
        for (const std::string& source : cookedLayerSources) {
            auto it = inputs.find(source);
            if (it == inputs.end()) continue;
            bool onlyLayers = !it->second.inference.uses.empty();
            for (const TextureReferenceUse& use : it->second.inference.uses) {
                if (use.where.find("TerrainComponent.surfaceLayers") != 0) onlyLayers = false;
            }
            if (onlyLayers) {
                droppedSources.insert(source);
                inputs.erase(it);
            }
        }
    }
    return true;
}
