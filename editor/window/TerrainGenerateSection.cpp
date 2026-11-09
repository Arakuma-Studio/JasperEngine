// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainGenerateSection.h"

#include "Backend.h"
#include "Project.h"
#include "component/TerrainComponent.h"
#include "external/IconsFontAwesome6.h"
#include "graph/Graph.h"
#include "noise/TerrainGeneration.h"
#include "noise/TerrainPresets.h"
#include "texture/Texture.h"
#include "util/ProjectChecks.h"
#include "util/UIProbe.h"
#include "util/Util.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    const char* const kPresetPrefix = "preset:";

    // A light from the north-west, what a terrain's shape reads best under
    std::vector<uint8_t> hillshade(const std::vector<float>& heights, int width, int depth, float metresPerTexel, float maxHeight) {
        std::vector<uint8_t> rgba(static_cast<size_t>(width) * static_cast<size_t>(depth) * 4);
        const float lx = -0.5f, ly = 0.7071f, lz = -0.5f;
        const float scale = maxHeight / std::max(metresPerTexel, 1e-3f);
        for (int z = 0; z < depth; z++) {
            for (int x = 0; x < width; x++) {
                const auto at = [&](int i, int j) {
                    return heights[static_cast<size_t>(std::clamp(j, 0, depth - 1)) * static_cast<size_t>(width) + static_cast<size_t>(std::clamp(i, 0, width - 1))];
                };
                const float dx = (at(x + 1, z) - at(x - 1, z)) * 0.5f * scale;
                const float dz = (at(x, z + 1) - at(x, z - 1)) * 0.5f * scale;
                const float length = std::sqrt(dx * dx + dz * dz + 1.0f);
                const float light = std::clamp((-dx * lx + ly - dz * lz) / length, 0.0f, 1.0f);
                const uint8_t grey = static_cast<uint8_t>((0.15f + 0.85f * light) * 255.0f);
                uint8_t* pixel = &rgba[(static_cast<size_t>(z) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4];
                pixel[0] = pixel[1] = pixel[2] = grey;
                pixel[3] = 255;
            }
        }
        return rgba;
    }

    class GenerateSection {
    public:
        void draw(Project* project, uint32_t sceneId, Entity entity);

    private:
        void releaseImage();
        void releaseRetired();
        void loadGraph(Project* project, const std::string& key);
        void drawGraphPicker(Project* project, TerrainEditorSettings& settings);
        void drawVariables();
        void start(Project* project, uint32_t sceneId, Entity entity, bool dryRun, const TerrainComponent& terrain);
        void takePreview(const GenerationResult& result, float terrainSize, float maxHeight);

        // what the combo shows
        std::vector<TerrainPreset> presets;
        std::vector<fs::path> graphFiles;

        std::string loadedKey;
        graph::Graph loaded;
        std::string loadedName;
        std::string loadError;
        std::vector<std::pair<std::string, float>> variables;

        GenerationApplyTo applyTo;
        bool rectangle = false;
        GenerationRegion region;
        float feather = 0.0f;

        std::shared_ptr<TerrainGeneration::Run> run;
        bool runIsPreview = false;
        std::string status;
        bool statusIsError = false;

        Texture preview;
        std::vector<Texture> retired;
        bool hasStats = false;
        GenerationStats stats;
        std::vector<float> histogram;
        uint64_t textureCounter = 0;
    };

    void GenerateSection::releaseRetired() {
        for (Texture& texture : retired) texture.destroy();
        retired.clear();
    }

    void GenerateSection::releaseImage() {
        if (!preview.empty()) retired.push_back(preview);
        preview = Texture();
        hasStats = false;
    }

    void GenerateSection::loadGraph(Project* project, const std::string& key) {
        if (key == loadedKey && loadError.empty()) return;
        loadedKey = key;
        loadError.clear();
        std::string text;
        if (key.rfind(kPresetPrefix, 0) == 0) {
            const std::string id = key.substr(std::strlen(kPresetPrefix));
            text = TerrainPresets::json(id);
            TerrainPreset preset;
            loadedName = TerrainPresets::find(id, preset) ? preset.name : id;
            if (text.empty()) loadError = "No preset '" + id + "'.";
        } else {
            const fs::path file = project->getAssetsPath() / key;
            loadedName = file.stem().string();
            if (!ProjectChecks::readFile(file, text)) loadError = "Could not read " + key + ".";
        }
        if (!loadError.empty()) return;
        graph::LoadReport report;
        loaded = graph::load(text, report);
        if (!report.ok) loadError = loadedName + " does not parse: " + report.error;
        std::vector<std::pair<std::string, float>> next;
        for (const graph::Variable& variable : loaded.variables) {
            if (!variable.exposed) continue;
            auto kept = std::find_if(variables.begin(), variables.end(), [&](const auto& entry) { return entry.first == variable.name; });
            next.push_back({variable.name, kept != variables.end() ? kept->second
                                         : variable.defaultValue.is_number() ? variable.defaultValue.get<float>() : 0.0f});
        }
        variables = std::move(next);
    }

    void GenerateSection::drawGraphPicker(Project* project, TerrainEditorSettings& settings) {
        const std::string name = loadError.empty() ? loadedName : std::string("(missing)");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        if (ImGui::BeginCombo("Noise Graph", name.c_str())) {
            if (ImGui::IsWindowAppearing()) {
                graphFiles.clear();
                project->getAssetDatabase().forEachAsset([&](const AssetDatabase::AssetInfo& asset) {
                    if (asset.type == AssetType::NoiseGraph) graphFiles.push_back(project->getProjectPath() / asset.path);
                });
                std::sort(graphFiles.begin(), graphFiles.end());
            }
            const auto choose = [&](const std::string& key) {
                settings.generateGraph = key;
                project->saveWorkspaceFile();
                releaseImage();
            };
            ImGui::TextDisabled("Presets");
            for (const TerrainPreset& preset : presets) {
                const std::string key = kPresetPrefix + preset.id;
                if (ImGui::Selectable(preset.name.c_str(), key == loadedKey)) choose(key);
                UIProbe::item("terrain.generate.preset." + preset.id);
                if (!preset.description.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", preset.description.c_str());
            }
            if (!graphFiles.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("Assets");
                for (const fs::path& file : graphFiles) {
                    const std::string key = project->isInsideAssetsPath(file) ? project->normalizeToAssetsRelative(file).generic_string() : file.generic_string();
                    if (ImGui::Selectable(file.filename().string().c_str(), key == loadedKey)) choose(key);
                }
            }
            ImGui::EndCombo();
        }
        UIProbe::item("terrain.generate.graph");
        // a .noisegraph dragged from the Project window
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
                const std::vector<std::string> dropped = Util::getStringsFromPayload(payload);
                if (!dropped.empty() && fs::path(dropped[0]).extension() == ".noisegraph" && project->isInsideAssetsPath(dropped[0])) {
                    settings.generateGraph = project->normalizeToAssetsRelative(dropped[0]).generic_string();
                    project->saveWorkspaceFile();
                    releaseImage();
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    void GenerateSection::drawVariables() {
        for (auto& [variable, value] : variables) ImGui::DragFloat(variable.c_str(), &value, 0.01f);
    }

    void GenerateSection::takePreview(const GenerationResult& result, float terrainSize, float maxHeight) {
        releaseImage();
        hasStats = true;
        stats = result.stats;
        histogram.assign(stats.slopeHistogram.begin(), stats.slopeHistogram.end());
        if (result.heights.empty() || result.width <= 0 || result.depth <= 0) return;
        const std::vector<uint8_t> rgba = hillshade(result.heights, result.width, result.depth, terrainSize / static_cast<float>(result.width), maxHeight);
        // TextureData frees what it is given (stbi_image_free = free)
        void* pixels = std::malloc(rgba.size());
        if (!pixels) return;
        std::memcpy(pixels, rgba.data(), rgba.size());
        TextureData data(result.width, result.depth, static_cast<unsigned int>(rgba.size()), ColorFormat::RGBA, 4, pixels);
        preview.setData("editor:terrain-generate:" + std::to_string(++textureCounter), data);
        preview.setMinFilter(TextureFilter::LINEAR);
        preview.setMagFilter(TextureFilter::LINEAR);
        TextureRender* render = preview.getRender();
        preview.releaseData();
        if (!render || !render->isCreated()) {
            retired.push_back(preview);
            preview = Texture();
        }
    }

    void GenerateSection::start(Project* project, uint32_t sceneId, Entity entity, bool dryRun, const TerrainComponent& terrain) {
        GenerationRequest request;
        request.graph = loaded;
        request.displayName = loadedName;
        request.seed = project->getTerrainEditorSettings().generateSeed;
        request.variables = variables;
        if (rectangle) request.region = region;
        request.feather = rectangle ? feather : 0.0f;
        request.applyTo = applyTo;
        request.dryRun = dryRun;
        request.interactive = dryRun;
        runIsPreview = dryRun;
        status.clear();
        statusIsError = false;
        project->saveWorkspaceFile();
        const float terrainSize = terrain.terrainSize;
        const float maxHeight = terrain.maxHeight;
        run = TerrainGeneration::start(project, sceneId, entity, std::move(request), [this, dryRun, terrainSize, maxHeight](const GenerationResult& result) {
            run.reset();
            if (result.cancelled) {
                status = "Cancelled: nothing was written.";
            } else if (!result.ok) {
                status = result.error;
                statusIsError = true;
            } else if (dryRun) {
                takePreview(result, terrainSize, maxHeight);
                status = "Preview: nothing was written.";
            } else {
                status = result.undoName;
            }
        });
    }

    void GenerateSection::draw(Project* project, uint32_t sceneId, Entity entity) {
        releaseRetired();
        const bool open = ImGui::CollapsingHeader("Generate");
        UIProbe::item("terrain.generate");
        if (!open) {
            // R50: collapsed, nothing is kept. A running Apply still finishes: its progress is back on the next open.
            releaseImage();
            return;
        }
        const SceneProject* scene = project->getScene(sceneId);
        const TerrainComponent* terrain = scene && scene->scene ? scene->scene->findComponent<TerrainComponent>(entity) : nullptr;
        if (!terrain) return;
        if (presets.empty()) presets = TerrainPresets::list();
        TerrainEditorSettings& settings = project->getTerrainEditorSettings();

        const std::string key = !settings.generateGraph.empty() ? settings.generateGraph
                              : !presets.empty() ? kPresetPrefix + presets.front().id : std::string();
        loadGraph(project, key);

        ImGui::PushID("terrain_generate");
        const bool busy = run != nullptr;
        ImGui::BeginDisabled(busy);
        drawGraphPicker(project, settings);

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
        uint32_t seed = settings.generateSeed;
        if (ImGui::InputScalar("Seed", ImGuiDataType_U32, &seed)) settings.generateSeed = seed;
        UIProbe::item("terrain.generate.seed");
        if (ImGui::IsItemDeactivatedAfterEdit()) project->saveWorkspaceFile();
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_DICE "##generate_dice")) {
            static std::random_device device;
            settings.generateSeed = std::uniform_int_distribution<uint32_t>(1, 1000000)(device);
            project->saveWorkspaceFile();
        }
        UIProbe::item("terrain.generate.dice");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("A new random seed");

        drawVariables();

        ImGui::TextUnformatted("Apply To");
        ImGui::SameLine();
        ImGui::Checkbox("Height", &applyTo.height);
        UIProbe::item("terrain.generate.apply_height");
        ImGui::SameLine();
        ImGui::Checkbox("Layers", &applyTo.layers);
        ImGui::SameLine();
        ImGui::Checkbox("Foliage Density", &applyTo.density);

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        if (ImGui::BeginCombo("Region", rectangle ? "Rectangle" : "Whole Terrain")) {
            if (ImGui::Selectable("Whole Terrain", !rectangle)) rectangle = false;
            if (ImGui::Selectable("Rectangle", rectangle)) {
                if (!rectangle && region.maxX <= region.minX) {
                    region.maxX = terrain->terrainSize;
                    region.maxZ = terrain->terrainSize;
                }
                rectangle = true;
            }
            ImGui::EndCombo();
        }
        UIProbe::item("terrain.generate.region");
        if (rectangle) {
            float min[2] = {region.minX, region.minZ};
            float max[2] = {region.maxX, region.maxZ};
            if (ImGui::DragFloat2("Min (X, Z)", min, 1.0f, 0.0f, terrain->terrainSize)) {
                region.minX = min[0];
                region.minZ = min[1];
            }
            if (ImGui::DragFloat2("Max (X, Z)", max, 1.0f, 0.0f, terrain->terrainSize)) {
                region.maxX = max[0];
                region.maxZ = max[1];
            }
            ImGui::DragFloat("Feather", &feather, 0.5f, 0.0f, terrain->terrainSize, "%.1f m");
        }

        const bool canRun = loadError.empty() && !project->isAnyScenePlaying();
        ImGui::BeginDisabled(!canRun);
        if (ImGui::Button("Preview##generate")) start(project, sceneId, entity, true, *terrain);
        UIProbe::item("terrain.generate.preview");
        ImGui::SameLine();
        if (ImGui::Button("Apply##generate")) start(project, sceneId, entity, false, *terrain);
        UIProbe::item("terrain.generate.apply");
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        if (run) {
            ImGui::ProgressBar(run->progress(), ImVec2(ImGui::GetContentRegionAvail().x * 0.6f, 0.0f), runIsPreview ? "Previewing" : "Generating");
            UIProbe::item("terrain.generate.progress");
            ImGui::SameLine();
            if (ImGui::Button("Cancel##generate")) run->cancel();
            UIProbe::item("terrain.generate.cancel");
        }
        if (!loadError.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", loadError.c_str());
        } else if (!status.empty()) {
            if (statusIsError) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", status.c_str());
            else ImGui::TextDisabled("%s", status.c_str());
            UIProbe::item("terrain.generate.status: " + status);
        }

        if (hasStats) {
            if (!preview.empty()) {
                if (TextureRender* render = preview.getRender(); render && render->isCreated()) {
                    const float side = std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 16.0f);
                    ImGui::Image(ImTextureRef(Backend::getImGuiTexture(render)), ImVec2(side, side));
                    UIProbe::item("terrain.generate.image");
                }
            }
            ImGui::Text("Min %.3f   Max %.3f", static_cast<double>(stats.heightMin), static_cast<double>(stats.heightMax));
            if (!histogram.empty()) {
                ImGui::PlotHistogram("Slope", histogram.data(), static_cast<int>(histogram.size()), 0, "0-90 degrees", 0.0f, FLT_MAX,
                                     ImVec2(ImGui::GetContentRegionAvail().x * 0.8f, ImGui::GetFontSize() * 3.0f));
            }
        }
        ImGui::PopID();
    }

}

void jasperengine::editor::drawTerrainGenerateSection(Project* project, uint32_t sceneId, Entity entity) {
    // The editor's life long, and not destroyed: its textures must not outlive the renderer at exit
    static GenerateSection* section = new GenerateSection();
    section->draw(project, sceneId, entity);
}
