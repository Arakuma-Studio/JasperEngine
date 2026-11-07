// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainSliceCook.h"

#include "Project.h"
#include "component/TerrainComponent.h"
#include "texture/TerrainDetailBuilder.h"
#include "texture/CookedTextures.h"
#include "util/PngWriter.h"

#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    // The fixed settings of a slice (R16): one format per family, so an array's slices can concatenate,
    // and at most the tier table's largest for that family
    void sliceSettings(TextureImportSettings& settings, terrain::DetailSliceKind kind) {
        settings.type = kind == terrain::DetailSliceKind::NORMAL ? TextureImportType::NormalMap : TextureImportType::Default;
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
    for (const SceneProject& sceneProject : project.getScenes()) {
        if (!sceneProject.scene) {
            allScenesLoaded = false;
            continue;
        }
        auto terrains = sceneProject.scene->getComponentArray<TerrainComponent>();
        for (size_t t = 0; t < terrains->size(); t++) {
            const TerrainComponent& component = terrains->getComponentFromIndex(t);
            const terrain::DetailPlan plan = terrain::planDetailArray(component);
            bool complete = plan.anySource;
            for (size_t s = 0; s < plan.sliceKeys.size() && complete; s++) {
                if (plan.sliceKeys[s].empty()) {
                    // a colour slot with no map stays white on the raw path: this array is not cooked
                    complete = false;
                    break;
                }
                const std::string key = terrain::cookedSliceIndexKey(plan.sliceKeys[s]);
                if (inputs.count(key)) continue;
                std::vector<uint8_t> rgba;
                int width = 0;
                int height = 0;
                if (!terrain::buildDetailSliceTop(component, plan, static_cast<int>(s), MAX_TERRAIN_DETAIL_SIZE, rgba, width, height)) {
                    complete = false;
                    break;
                }
                TextureCookInputs in;
                in.assetPath = key;
                in.absolute = dir / fs::path(key).filename();
                in.projectRelative = project.normalizeToProjectRelative(in.absolute).generic_string();
                if (!writePng(in.absolute, width, height, 4, rgba.data(), width * 4, io::Durability::Cache)) {
                    error = "cannot write " + in.absolute.generic_string();
                    return false;
                }
                sliceSettings(in.settings, terrain::detailSliceKind(plan, static_cast<int>(s)));
                in.hasSettings = true;
                in.inference.linear = true;
                inputs.emplace(key, std::move(in));
            }
            if (!complete) continue;
            for (const TerrainSurfaceLayer& layer : component.surfaceLayers) {
                for (const Texture* texture : {&layer.colorTexture, &layer.normalTexture, &layer.roughnessTexture, &layer.metallicTexture, &layer.occlusionTexture, &layer.heightTexture}) {
                    if (!texture->empty()) cookedLayerSources.insert(CookedTextures::normalize(texture->getPath(0)));
                }
            }
        }
    }

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
