// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Title3DExport.h"

#include "Project.h"
#include "Title3DEditor.h"
#include "TitleEnvironmentCook.h"
#include "asset/AssetFileWrite.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtRenderHost.h"
#include "titleart/three_d/Title3DStyle.h"
#include "titleart/three_d/TitleCooked3D.h"
#include "titleart/three_d/TitleEnvironment.h"
#include "titleart/three_d/TitleTierFeatures.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;

namespace fs = std::filesystem;

namespace {

    const char* const PLATFORMS[6] = {"windows", "linux", "macos", "ios", "android", "web"};

    std::shared_ptr<const titleart::TitleArtDoc> readDocument(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return nullptr;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        titleart::TitleArtParseResult parsed = titleart::parseTitleArt(text);
        if (!parsed.ok) return nullptr;
        return std::make_shared<const titleart::TitleArtDoc>(std::move(parsed.doc));
    }

    bool has3D(const titleart::TitleArtDoc& doc) {
        bool found = false;
        titleart::forEachLayer(doc.root, [&found](const Json& layer, const Json*, int) {
            found = found || layer.value("dimension", std::string()) == "3d";
            return !found;
        });
        return found;
    }

    // the texture families a platform's build loads (TextureCookQueue::familiesForExport's rows)
    std::vector<TextureFamily> familiesOf(const std::string& platform) {
        if (platform == "web") return {TextureFamily::Web};
        if (platform == "ios") return {TextureFamily::Mobile};
        if (platform == "android") return {TextureFamily::Mobile, TextureFamily::AndroidEtc2};
        return {TextureFamily::Desktop};
    }

    bool write(const fs::path& target, const std::vector<uint8_t>& bytes, Title3DCookResult& result) {
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        std::string error;
        if (!assetio::writeFile(target, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), assetio::Durability::Cache,
                                &error)) {
            result.ok = false;
            result.error = "cannot write " + target.string() + ": " + error;
            return false;
        }
        result.bytes += bytes.size();
        return true;
    }

}

int editor::titleArt3DTierFor(Project* project, const std::string& qualityTier, const std::string& platform) {
    titleart::TitleTier projectTier = titleart::TitleTier::High;
    if (project) titleart::titleTierFromName(project->getTitleArtSettings().realtime3DQuality.of(platform), projectTier);
    if (projectTier == titleart::TitleTier::Bake) projectTier = titleart::TitleTier::High;
    return (int)titleart::resolveTitleTier(qualityTier, projectTier, false, DeviceTier::High, platform == "web");
}

std::vector<int> editor::titleArt3DPrebakeTiers(Project* project, const titleart::TitleArtDoc& doc, const std::string& qualityTier,
                                                const std::vector<std::string>& platforms) {
    if (!has3D(doc)) return {0};
    std::set<int> tiers;
    for (const std::string& platform : platforms) tiers.insert(titleArt3DTierFor(project, qualityTier, platform));
    if (tiers.empty()) tiers.insert(titleArt3DTierFor(project, qualityTier, "linux"));
    return std::vector<int>(tiers.begin(), tiers.end());
}

std::vector<std::string> editor::titleArt3DEnvironments(const titleart::TitleArtDoc& doc) {
    std::set<std::string> presets;
    const titleart::Scene3D scene = titleart::decodeScene3D(doc.root);
    titleart::forEachLayer(doc.root, [&](const Json& layer, const Json*, int) {
        if (layer.value("dimension", std::string()) != "3d") return true;
        const titleart::Title3D style = titleart::decodeStyle3D(layer);
        presets.insert(titleart::resolveLighting(scene, style).environment.preset);
        auto part = [&](const titleart::TitleMaterial3D& m) { presets.insert(titleart::resolveLighting(scene, style, &m).environment.preset); };
        part(style.face);
        if (style.bevelMaterial) part(*style.bevelMaterial);
        if (style.side) part(*style.side);
        for (const titleart::Band3D& band : style.bands) {
            part(band.material);
            if (band.side) part(*band.side);
        }
        return true;
    });
    return std::vector<std::string>(presets.begin(), presets.end());
}

Title3DCookResult editor::cookTitleArt3D(Project* project, const TitleArtExportPlan& plan, const fs::path& assetsDst,
                                         const std::vector<std::string>& platforms, const std::vector<TextureFamily>& families) {
    Title3DCookResult result;
    // the bundled HDRIs' loader (a command-line build registers no MCP tools, which register it)
    registerTitle3DEditor();
    const fs::path cookedDir = assetsDst / titleart::TITLE_COOKED_DIR;
    std::error_code ec;
    fs::remove_all(cookedDir, ec);
    if (!project) return result;

    struct Title {
        std::shared_ptr<const titleart::TitleArtDoc> doc;
        float resolution = 0.0f;
        std::set<std::string> environments;
    };
    std::map<std::string, Title> titles;
    // the best tier (the smallest number) each family's cube must serve, per preset
    std::map<std::pair<TextureFamily, std::string>, int> cubes;
    for (const TitleArtShippedInstance& instance : plan.instances) {
        if (instance.mode == TitleArtRenderMode::BAKED) continue;
        auto it = titles.find(instance.title);
        if (it == titles.end()) {
            Title t;
            t.doc = readDocument(project->getProjectPath() / fs::path(instance.title));
            if (!t.doc || !has3D(*t.doc)) {
                titles[instance.title] = Title();
                continue;
            }
            for (const std::string& preset : titleArt3DEnvironments(*t.doc)) t.environments.insert(preset);
            it = titles.emplace(instance.title, std::move(t)).first;
        }
        Title& title = it->second;
        if (!title.doc) continue;
        title.resolution = std::max(title.resolution, instance.resolution);
        for (const std::string& preset : instance.runtimeEnvironments) {
            if (titleart::titleEnvironmentPreset(preset)) {
                title.environments.insert(preset);
            } else {
                result.warnings.push_back("W-TITLEART-3D-ENV: Title Art " + instance.object + " (" + instance.where + "): Runtime Environments names '" +
                                          preset + "', which is no title environment: it is not cooked");
            }
        }
        for (const std::string& platform : platforms) {
            const int tier = titleArt3DTierFor(project, instance.qualityTier, platform);
            for (TextureFamily family : familiesOf(platform)) {
                if (std::find(families.begin(), families.end(), family) == families.end()) continue;
                for (const std::string& preset : title.environments) {
                    auto [cube, added] = cubes.emplace(std::make_pair(family, preset), tier);
                    if (!added) cube->second = std::min(cube->second, tier);
                }
            }
        }
    }

    Json index = Json{{"version", titleart::TITLE_COOKED_INDEX_VERSION}, {"titles", Json::object()}, {"environments", Json::object()}};
    Json quality = Json::object();
    for (const char* platform : PLATFORMS) quality[platform] = project->getTitleArtSettings().realtime3DQuality.of(platform);
    index["realtime_3d_quality"] = quality;
    const titleart::AssetResolver* assets = TitleArtRenderHost::editor().resolver(project);
    bool any = false;
    for (const auto& [path, title] : titles) {
        if (!title.doc) continue;
        any = true;
        Json entry = Json{{"meshes", Json::object()}, {"environments", Json::array()}};
        for (const std::string& preset : title.environments) {
            if (preset != "matcap") entry["environments"].push_back(preset);
        }
        // R38: the layers whose geometry is fixed, at every Realtime tier
        std::set<std::string> fixed;
        titleart::forEachLayer(title.doc->root, [&](const Json& layer, const Json*, int) {
            if (layer.value("dimension", std::string()) == "3d" && titleart::titleArt3DFixedGeometry(title.doc->root, layer, nullptr))
                fixed.insert(layer.value("id", std::string()));
            return true;
        });
        if (!fixed.empty()) {
            for (titleart::TitleTier tier : {titleart::TitleTier::High, titleart::TitleTier::Medium, titleart::TitleTier::Low}) {
                std::map<std::string, std::vector<uint8_t>> meshes;
                std::string error;
                if (!cookTitleMeshes(*title.doc, assets, tier, std::max(0.125f, title.resolution), &fixed, meshes, error)) {
                    result.warnings.push_back("W-TITLEART-3D-MESH: " + path + ": no cooked mesh (" + error +
                                              "): a build without the mesher draws nothing for that layer");
                    break;
                }
                for (const auto& [layerId, bytes] : meshes) {
                    const std::string file = titleart::titleCookedMeshFile(path, layerId, tier);
                    if (!write(cookedDir / file, bytes, result)) return result;
                    entry["meshes"][layerId][titleart::titleTierName(tier)] = file;
                    result.written.push_back(std::string(titleart::TITLE_COOKED_DIR) + "/" + file);
                    result.meshes++;
                }
            }
        }
        index["titles"][path] = entry;
    }
    if (!any) return result;

    // R26: the environment cubes, one per (family, preset)
    for (const auto& [key, tier] : cubes) {
        const auto& [family, preset] = key;
        if (preset == "matcap") continue;
        std::vector<uint8_t> ktx2;
        std::string error;
        if (!cookTitleEnvironment(preset, (titleart::TitleTier)tier, family, ktx2, error)) {
            result.warnings.push_back("W-TITLEART-3D-ENV: the environment '" + preset + "' was not cooked for " + textureFamilyName(family) + " (" +
                                      error + "): 3D titles there light with the studio");
            continue;
        }
        const std::string file = titleart::titleCookedEnvironmentFile(textureFamilyName(family), preset);
        if (!write(cookedDir / file, ktx2, result)) return result;
        index["environments"][textureFamilyName(family)][preset] = file;
        result.written.push_back(std::string(titleart::TITLE_COOKED_DIR) + "/" + file);
        result.environments++;
    }
    const std::string text = index.dump(1);
    if (!write(cookedDir / titleart::TITLE_COOKED_INDEX_FILE, std::vector<uint8_t>(text.begin(), text.end()), result)) return result;
    result.written.push_back(std::string(titleart::TITLE_COOKED_DIR) + "/" + titleart::TITLE_COOKED_INDEX_FILE);
    return result;
}
