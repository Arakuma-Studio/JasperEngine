// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Terrain window's Terrain Settings tool, its issues banner, Import Raw... / Export Raw... and
// the Terrain Info panel (T1 R59, R61, R62, R72). Every write is an undo step, and the heights go
// through the same ops and undo steps edit_terrain uses (terrain/TerrainOpsUndo.h).

#include "TerrainEditWindow.h"
#include "component/TerrainComponent.h"

#include "App.h"
#include "Backend.h"
#include "Out.h"
#include "Theme.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/PropertyCmd.h"
#include "external/IconsFontAwesome6.h"
#include "math/AABB.h"
#include "object/TerrainEdit.h"
#include "sceneview/SceneViewController.h"
#include "terrain/TerrainIssues.h"
#include "terrain/TerrainLayers.h"
#include "terrain/TerrainOpsUndo.h"
#include "util/Angle.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "util/FloatImage.h"
#include "util/TerrainMapFileWriter.h"
#include "util/UIUtils.h"
#include "window/EditorModal.h"
#include "window/HeightImportFields.h"
#include "window/SceneWindow.h"

#include <algorithm>
#include <cerrno>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iterator>

using namespace jasperengine;
using namespace jasperengine::editor;
using terrain::MapId;
using terrain::MapKind;

namespace {

    bool settingsTableBegin(const char* id) {
        if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) return false;
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, std::min(ImGui::GetFontSize() * 9.0f, ImGui::GetContentRegionAvail().x * 0.45f));
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        return true;
    }

    // A row's label; true when it was clicked (R59: a click on the label resets the value)
    bool settingsRow(const char* label, const char* tooltip = nullptr) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (tooltip) ImGui::SetItemTooltip("%s", tooltip);
        const bool clicked = ImGui::IsItemClicked();
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1);
        return clicked;
    }

    std::string settingsBytes(double bytes) {
        char text[32];
        if (bytes >= 1024.0 * 1024.0) std::snprintf(text, sizeof(text), "%.1f MB", bytes / (1024.0 * 1024.0));
        else std::snprintf(text, sizeof(text), "%.0f KB", bytes / 1024.0);
        return text;
    }

    ImVec4 severityColor(TerrainIssueSeverity severity) {
        switch (severity) {
            case TerrainIssueSeverity::Error: return ImVec4(0.95f, 0.35f, 0.3f, 1.0f);
            case TerrainIssueSeverity::Warning: return ImVec4(0.95f, 0.75f, 0.25f, 1.0f);
            case TerrainIssueSeverity::Info: break;
        }
        return ImVec4(0.6f, 0.75f, 0.95f, 1.0f);
    }

}

template<typename T>
bool editor::TerrainEditWindow::setTerrainProperty(const char* property, const T& value){
    SceneProject* sceneProject = getTargetSceneProject();
    if (!hasValidTarget(sceneProject)){
        return false;
    }
    CommandHandle::get(sceneProject->id)->addCommandNoMerge(new PropertyCmd<T>(project, sceneProject->id, selectedEntity,
                                                                               ComponentType::TerrainComponent, property, value));
    return true;
}

void editor::TerrainEditWindow::drawIssuesBanner(SceneProject* sceneProject, TerrainComponent& terrain){
    const std::vector<TerrainIssue> issues = TerrainIssues::check(terrain, TerrainMapFileWriter::get().failedCount());
    for (size_t i = 0; i < issues.size(); i++){
        const TerrainIssue& issue = issues[i];
        if (issue.severity == TerrainIssueSeverity::Info && issue.fix == TerrainIssueFix::None) continue;
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Text, severityColor(issue.severity));
        ImGui::TextWrapped("%s %s", issue.severity == TerrainIssueSeverity::Error ? ICON_FA_CIRCLE_EXCLAMATION : ICON_FA_TRIANGLE_EXCLAMATION,
                           issue.message.c_str());
        ImGui::PopStyleColor();
        if (issue.fix != TerrainIssueFix::None && ImGui::SmallButton(issue.fixLabel.c_str())){
            std::string error;
            switch (issue.fix){
                case TerrainIssueFix::CreateTerrain:
                    createMapForTarget(TerrainMapTarget::HeightMap, heightMapResolution, heightMapResolution);
                    break;
                case TerrainIssueFix::RetryLoad:
                    terrain.heightMapLoaded = false;
                    terrain.needUpdateTerrain = true;
                    break;
                case TerrainIssueFix::RetryWrite:
                    TerrainMapFileWriter::get().flushAll();
                    break;
                case TerrainIssueFix::ConvertTo16Bit:
                    convertHeightMapTo16Bit(sceneProject->id, selectedEntity, error);
                    break;
                case TerrainIssueFix::RaiseTerrainHeight:
                    endStroke();
                    brushActive = false;
                    toolCategory = TerrainToolCategory::Settings;
                    break;
                case TerrainIssueFix::None:
                    break;
            }
        }
        ImGui::PopID();
    }
}

bool editor::TerrainEditWindow::setTerrainHeight(SceneProject* sceneProject, float height, std::string& error){
    TerrainComponent& terrain = sceneProject->scene->getComponent<TerrainComponent>(selectedEntity);
    const float before = terrain.maxHeight;
    if (height == before || height <= 0.0f || !std::isfinite(height)){
        return true;
    }
    CommandHistory* history = CommandHandle::get(sceneProject->id);
    const size_t start = history->mark();
    setTerrainProperty("maxHeight", height);
    if (keepWorldHeights && !terrain.heightMap.empty()){
        // the stored 0-1 heights rescale so each texel keeps its metres (clamped at the new top)
        terrain::Op remap;
        remap.type = terrain::OpType::RemapHeights;
        remap.inRange = terrain::FilterRange{0.0f, height};
        remap.outRange = terrain::FilterRange{0.0f, before};
        const std::vector<terrain::Op> ops = {remap};
        const terrain::TerrainOpResult preview = previewTerrainOps(project, sceneProject, selectedEntity, ops, terrain::OpOptions());
        terrain::TerrainOpResult reported;
        if (!preview.ok || !applyTerrainOpsWithUndo(project, sceneProject, selectedEntity, ops, terrain::OpOptions(), preview, "Terrain Height", reported, error)){
            if (error.empty()) error = preview.error;
            while (history->position() > start && history->canUndo() && history->undo()){}
            return false;
        }
    }
    history->groupSince(start, "Terrain Height");
    return true;
}

void editor::TerrainEditWindow::frameTerrain(SceneProject* sceneProject){
    SceneWindow* window = Backend::getApp().getSceneWindow();
    TerrainComponent* terrain = sceneProject->scene->findComponent<TerrainComponent>(selectedEntity);
    const TerrainWorldPlacement placement = terrainWorldPlacement(sceneProject->scene, selectedEntity);
    if (!window || !terrain || !placement.valid) return;
    AABB box;
    const float half = terrain->terrainSize * 0.5f;
    for (int corner = 0; corner < 8; corner++){
        box.merge(placement.model * Vector3((corner & 1) ? half : -half, (corner & 2) ? terrain->maxHeight : 0.0f, (corner & 4) ? half : -half));
    }
    SceneViewController& view = window->getViewController();
    const SceneCameraPose pose = view.currentPose(*sceneProject);
    Vector3 direction = pose.target - pose.position;
    if (direction.squaredLength() < 1e-12f) direction = Vector3(0.0f, -0.5f, -1.0f);
    direction.normalize();
    const float radius = std::max(box.getSize().length() * 0.5f, 0.5f);
    const float halfFov = sceneProject->viewState.camera.fieldOfView * 0.5f * 3.14159265f / 180.0f;
    const float distance = radius * 1.1f / std::max(0.05f, std::sin(halfFov));
    view.lookFrom(*sceneProject, box.getCenter() - direction * distance, box.getCenter(), Vector3(0.0f, 1.0f, 0.0f));
}

void editor::TerrainEditWindow::drawSettingsTool(SceneProject* sceneProject, TerrainComponent& terrain){
    std::string error;
    // a click on a row's label sets the value a new component has
    const TerrainComponent defaults{};
    if (Theme::foldoutHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen) && settingsTableBegin("terrain_settings_mesh")){
        float size = terrain.terrainSize;
        if (settingsRow("Terrain Size", "Width and length in metres. Click the label to reset it.")) setTerrainProperty("terrainSize", defaults.terrainSize);
        ImGui::DragFloat("##terrain_size", &size, 1.0f, 1.0f, 100000.0f, "%.1f m");
        if (ImGui::IsItemDeactivatedAfterEdit()) setTerrainProperty("terrainSize", std::max(1.0f, size));

        float height = terrain.maxHeight;
        if (settingsRow("Terrain Height", "Height in metres of the top of the height map. Click the label to reset it.")){
            if (!setTerrainHeight(sceneProject, defaults.maxHeight, error)) Out::error("%s", error.c_str());
        }
        ImGui::DragFloat("##terrain_height", &height, 0.5f, 0.01f, 100000.0f, "%.2f m");
        if (ImGui::IsItemDeactivatedAfterEdit() && !setTerrainHeight(sceneProject, std::max(0.01f, height), error)) Out::error("%s", error.c_str());
        settingsRow("Keep World Heights", "A new Terrain Height rescales the map so the surface stays where it is.");
        ImGui::Checkbox("##keep_world_heights", &keepWorldHeights);

        float resolution = terrain.resolution;
        if (settingsRow("Mesh Resolution", "Mesh cells per root node side.")) setTerrainProperty("resolution", defaults.resolution);
        ImGui::DragFloat("##mesh_resolution", &resolution, 1.0f, 1.0f, 512.0f, "%.0f");
        if (ImGui::IsItemDeactivatedAfterEdit()) setTerrainProperty("resolution", std::max(1.0f, resolution));
        int rootGrid = terrain.rootGridSize;
        if (settingsRow("Root Grid Size")) setTerrainProperty("rootGridSize", defaults.rootGridSize);
        if (ImGui::InputInt("##root_grid", &rootGrid) && rootGrid >= 1) setTerrainProperty("rootGridSize", std::clamp(rootGrid, 1, 64));
        int levels = terrain.levels;
        if (settingsRow("Levels")) setTerrainProperty("levels", defaults.levels);
        if (ImGui::InputInt("##levels", &levels) && levels >= 1) setTerrainProperty("levels", std::clamp(levels, 1, 16));
        bool autoRanges = terrain.autoSetRanges;
        settingsRow("Auto Ranges", "Level ranges follow the camera's view distance.");
        if (ImGui::Checkbox("##auto_ranges", &autoRanges)) setTerrainProperty("autoSetRanges", autoRanges);
        ImGui::EndTable();
    }

    TerrainComponentStore store(sceneProject->scene, selectedEntity, terrain);
    if (Theme::foldoutHeader("Maps", ImGuiTreeNodeFlags_DefaultOpen) && settingsTableBegin("terrain_settings_maps")){
        std::vector<std::pair<MapId, std::string>> maps = {{MapId{MapKind::Height, 0}, "Height Map"}, {MapId{MapKind::Hole, 0}, "Hole Map"}};
        for (size_t i = 0; i < terrain.blendMaps.size(); i++) maps.push_back({MapId{MapKind::Blend, static_cast<int>(i)}, "Blend Map " + std::to_string(i)});
        for (size_t i = 0; i < terrain.foliageLayers.size(); i++) maps.push_back({MapId{MapKind::Density, static_cast<int>(i)}, "Details " + std::to_string(i)});
        for (const auto& [map, name] : maps){
            const terrain::MapFormat format = store.format(map);
            if (!format.exists) continue;
            settingsRow(name.c_str());
            Texture* texture = terrainMapTexture(terrain, map);
            const std::string file = texture ? texture->getPath(0) : std::string();
            ImGui::Text("%d x %d, %d-bit%s%s", format.bounds.w, format.bounds.h, format.bitsPerChannel, file.empty() ? "" : ", ",
                        file.empty() ? "" : FileUtils::pathToUtf8(std::filesystem::path(file).filename()).c_str());
            if (map.kind == MapKind::Height && format.bitsPerChannel < 16){
                ImGui::SameLine();
                if (ImGui::SmallButton("Convert to 16-bit") && !convertHeightMapTo16Bit(sceneProject->id, selectedEntity, error)) Out::error("%s", error.c_str());
            }
            if (map.kind == MapKind::Blend){
                ImGui::SameLine();
                ImGui::PushID(name.c_str());
                if (ImGui::SmallButton("Remove")) deleteMapForTarget(TerrainMapRef(TerrainMapTarget::BlendMap, map.index));
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    if (Theme::foldoutHeader("Height Map", ImGuiTreeNodeFlags_DefaultOpen) && settingsTableBegin("terrain_settings_io")){
        settingsRow("Import Raw...", "A .raw, .r16, .r8, .r32, 16-bit .png or .exr heightmap; its size, depth and byte order are detected.");
        if (ImGui::Button("Import Raw...")){
            const std::string chosen = FileDialogs::openFileDialogFiltered(project->getAssetsPath().string(), "Heightmaps", "raw,r16,r8,r32,f32,png,exr");
            if (!chosen.empty()){
                importRaw = ImportRawDialog();
                importRaw.path = chosen;
                std::ifstream file(std::filesystem::u8path(chosen), std::ios::binary);
                importRaw.bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
                importRaw.preview = TerrainRawIO::decode(chosen, importRaw.bytes, importRaw.options);
                importRaw.open = true;
                importRaw.justOpened = true;
            }
        }
        settingsRow("Export Raw...", "Write the height map as a raw or image file.");
        ImGui::BeginDisabled(terrain.heightMap.empty());
        if (ImGui::Button("Export Raw...")){
            exportRaw = ExportRawDialog();
            exportRaw.open = true;
            exportRaw.justOpened = true;
        }
        ImGui::EndDisabled();
        ImGui::EndTable();
    }

    if (Theme::foldoutHeader("Collision", ImGuiTreeNodeFlags_DefaultOpen) && settingsTableBegin("terrain_settings_collision")){
        const terrain::MapFormat height = store.format(MapId{MapKind::Height, 0});
        settingsRow("Samples");
        if (height.exists){
            const int samples = std::min(height.bounds.w, 1025);
            ImGui::Text("%d x %d", samples, samples);
            if (samples < height.bounds.w){
                ImGui::PushStyleColor(ImGuiCol_Text, severityColor(TerrainIssueSeverity::Warning));
                ImGui::TextWrapped("The collider resamples the %d-texel map: small bumps may not collide.", height.bounds.w);
                ImGui::PopStyleColor();
            }
        }else{
            ImGui::TextUnformatted("No height map");
        }
        ImGui::EndTable();
    }

    if (Theme::foldoutHeader("Runtime", ImGuiTreeNodeFlags_DefaultOpen) && settingsTableBegin("terrain_settings_runtime")){
        bool allow = terrain.allowRuntimeEdits;
        settingsRow("Allow Runtime Edits", "A game may change this terrain with Terrain:applyOps and setHeights (docs/scripting/terrain.md).");
        if (ImGui::Checkbox("##allow_runtime_edits", &allow)) setTerrainProperty("allowRuntimeEdits", allow);
        double bytes = 0.0;
        for (const MapId& map : {MapId{MapKind::Height, 0}, MapId{MapKind::Hole, 0}}){
            const terrain::MapFormat format = store.format(map);
            if (format.exists) bytes += static_cast<double>(format.bounds.area()) * format.channels * (format.bitsPerChannel / 8);
        }
        for (size_t i = 0; i < terrain.blendMaps.size(); i++){
            const terrain::MapFormat format = store.format(MapId{MapKind::Blend, static_cast<int>(i)});
            if (format.exists) bytes += static_cast<double>(format.bounds.area()) * format.channels * (format.bitsPerChannel / 8);
        }
        settingsRow("Memory", "What the maps keep on the CPU for runtime edits.");
        ImGui::TextUnformatted(allow ? settingsBytes(bytes).c_str() : "None while off");
        ImGui::EndTable();
    }

    if (ImGui::Button(ICON_FA_EXPAND " Frame Terrain")){
        frameTerrain(sceneProject);
    }
    drawImportRawDialog(sceneProject, terrain);
    drawExportRawDialog(sceneProject, terrain);
}

void editor::TerrainEditWindow::drawImportRawDialog(SceneProject* sceneProject, TerrainComponent& terrain){
    if (importRaw.justOpened){
        ImGui::OpenPopup("Import Raw Heightmap");
        importRaw.justOpened = false;
    }
    if (!EditorModal::begin("Import Raw Heightmap", &importRaw.open, ImGuiWindowFlags_AlwaysAutoResize)){
        return;
    }
    EditorModal::message("Import " + FileUtils::pathToUtf8(std::filesystem::u8path(importRaw.path).filename()) +
                         " as the height map: depth, resolution and byte order are detected, the marked ones guessed.");
    EditorModal::error(importRaw.preview.ok ? importRaw.message : importRaw.preview.error);
    drawHeightImportFields(importRaw.path, importRaw.bytes, importRaw.options, importRaw.preview, terrain.maxHeight, &importRaw.setHeightFromRange);
    const HeightImport& p = importRaw.preview;
    if (!importRaw.message.empty()){
        ImGui::TextWrapped("%s", importRaw.message.c_str());
    }

    ImGui::BeginDisabled(!p.ok);
    if (EditorModal::button("Import", "ok")){
        std::string error;
        if (importRawFile(sceneProject, error)){
            importRaw.open = false;
            ImGui::CloseCurrentPopup();
        }else{
            importRaw.message = error;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (EditorModal::button("Cancel", "cancel")){
        importRaw.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

bool editor::TerrainEditWindow::importRawFile(SceneProject* sceneProject, std::string& error){
    const HeightImport& p = importRaw.preview;
    CommandHistory* history = CommandHandle::get(sceneProject->id);
    const size_t start = history->mark();
    auto rollback = [&](){
        while (history->position() > start && history->canUndo() && history->undo()){}
    };
    TerrainComponent* terrain = &sceneProject->scene->getComponent<TerrainComponent>(selectedEntity);
    TerrainComponentStore store(sceneProject->scene, selectedEntity, *terrain);
    const terrain::MapFormat format = store.format(MapId{MapKind::Height, 0});
    // R61: another size is a new map (the structural path); the same size is a delta
    if (!format.exists || format.bounds.w != p.width || format.bounds.h != p.height){
        if (!createMapForTarget(TerrainMapTarget::HeightMap, p.width, p.height)){
            error = "A " + std::to_string(p.width) + " x " + std::to_string(p.height) + " height map could not be made.";
            rollback();
            return false;
        }
    }
    if (importRaw.setHeightFromRange && importRaw.options.range == HeightRange::Source && importRaw.options.sourceMax > importRaw.options.sourceMin){
        setTerrainProperty("maxHeight", importRaw.options.sourceMax - importRaw.options.sourceMin);
    }
    terrain::Op op;
    op.type = terrain::OpType::ImportHeightmap;
    op.importedWidth = p.width;
    op.importedHeight = p.height;
    op.importedHeights = p.normalized;
    const std::vector<terrain::Op> ops = {op};
    const terrain::TerrainOpResult preview = previewTerrainOps(project, sceneProject, selectedEntity, ops, terrain::OpOptions());
    terrain::TerrainOpResult reported;
    if (!preview.ok || !applyTerrainOpsWithUndo(project, sceneProject, selectedEntity, ops, terrain::OpOptions(), preview, "Import Heightmap", reported, error)){
        if (error.empty()) error = preview.error;
        rollback();
        return false;
    }
    history->groupSince(start, "Import Heightmap");
    return true;
}

void editor::TerrainEditWindow::drawExportRawDialog(SceneProject* sceneProject, TerrainComponent& terrain){
    if (exportRaw.justOpened){
        ImGui::OpenPopup("Export Raw Heightmap");
        exportRaw.justOpened = false;
    }
    if (!EditorModal::begin("Export Raw Heightmap", &exportRaw.open, ImGuiWindowFlags_AlwaysAutoResize)){
        return;
    }
    EditorModal::message("Export the height map to a file; Export... asks where.");
    const HeightFileFormat formats[] = {HeightFileFormat::R16, HeightFileFormat::R8, HeightFileFormat::R32F, HeightFileFormat::Png, HeightFileFormat::Exr};
    int depth = 0;
    for (int i = 0; i < IM_ARRAYSIZE(formats); i++){
        if (formats[i] == exportRaw.options.format) depth = i;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    if (EditorModal::combo("Depth", "depth", &depth, {"Bit 16", "Bit 8", "Float 32", "PNG", "EXR"})) exportRaw.options.format = formats[depth];
    int order = exportRaw.options.byteOrder == HeightByteOrder::Big ? 1 : 0;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    if (EditorModal::combo("Byte Order", "byte_order", &order, {"Windows", "Mac"})) exportRaw.options.byteOrder = order ? HeightByteOrder::Big : HeightByteOrder::Little;
    EditorModal::checkbox("Flip Vertically", "flip_vertically", &exportRaw.options.flipVertically);
    int range = exportRaw.options.range == HeightRange::Auto ? 1 : 0;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    if (EditorModal::combo("Range", "range", &range, {"Full", "Detected"})) exportRaw.options.range = range ? HeightRange::Auto : HeightRange::Full;

    if (EditorModal::button("Export...", "export")){
        const char* extension = TerrainRawIO::formatName(exportRaw.options.format);
        const std::string chosen = FileDialogs::saveFileDialog(project->getAssetsPath().string(),
                                                               sceneProject->scene->getEntityName(selectedEntity) + "." + extension, "Heightmap", extension);
        if (!chosen.empty()){
            TerrainComponentStore store(sceneProject->scene, selectedEntity, terrain);
            const MapId heightMap{MapKind::Height, 0};
            const terrain::MapFormat format = store.format(heightMap);
            std::vector<float> heights(static_cast<size_t>(format.bounds.area()));
            store.read(heightMap, format.bounds, heights);
            std::string error;
            bool written = false;
            if (exportRaw.options.format == HeightFileFormat::Exr){
                FloatImage image;
                image.width = format.bounds.w;
                image.height = format.bounds.h;
                image.rgba.resize(static_cast<size_t>(image.width) * image.height * 4);
                for (int y = 0; y < image.height; y++){
                    const int row = exportRaw.options.flipVertically ? image.height - 1 - y : y;
                    for (int x = 0; x < image.width; x++){
                        float* texel = image.rgba.data() + (static_cast<size_t>(y) * image.width + x) * 4;
                        texel[0] = texel[1] = texel[2] = heights[static_cast<size_t>(row) * image.width + x];
                        texel[3] = 1.0f;
                    }
                }
                written = writeExr(chosen, image, false, &error);
            }else{
                std::vector<uint8_t> bytes;
                written = TerrainRawIO::encode(heights, format.bounds.w, format.bounds.h, exportRaw.options, bytes, error);
                if (written && !FileUtils::writeFileReplacing(std::filesystem::u8path(chosen), std::string(bytes.begin(), bytes.end()))){
                    written = false;
                    error = std::strerror(errno);
                }
            }
            exportRaw.message = written ? "Wrote " + std::to_string(format.bounds.w) + " x " + std::to_string(format.bounds.h) + " to " + chosen + "."
                                        : "Not written: " + error;
        }
    }
    ImGui::SameLine();
    if (EditorModal::button("Close", "cancel")){
        exportRaw.open = false;
        ImGui::CloseCurrentPopup();
    }
    if (!exportRaw.message.empty()){
        ImGui::TextWrapped("%s", exportRaw.message.c_str());
    }
    ImGui::EndPopup();
}

void editor::TerrainEditWindow::drawInfoPanel(SceneProject* sceneProject, TerrainComponent& terrain){
    if (!Theme::foldoutHeader("Terrain Info", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (!infoValid || infoEntity != selectedEntity){
        ImGui::TextDisabled("Point at the terrain in the Scene view.");
        return;
    }
    const terrain::SurfaceSample sample = querySurface(terrain, infoLocal.x, infoLocal.z);
    const Vector3 world = terrainWorldPlacement(sceneProject->scene, selectedEntity).model * Vector3(infoLocal.x, sample.height, infoLocal.z);
    if (!settingsTableBegin("terrain_info")) return;
    settingsRow("Position");
    ImGui::Text("%.2f, %.2f, %.2f", world.x, world.y, world.z);
    settingsRow("Height");
    ImGui::Text("%.2f m (%.1f%%)", world.y, terrain.maxHeight != 0.0f ? sample.height / terrain.maxHeight * 100.0f : 0.0f);
    settingsRow("Slope");
    ImGui::Text("%.1f deg", Angle::radToDeg(std::acos(std::clamp(sample.normal.y, -1.0f, 1.0f))));
    settingsRow("Hole");
    ImGui::TextUnformatted(sample.hole ? "Yes" : "No");

    TerrainComponentStore store(sceneProject->scene, selectedEntity, terrain);
    const terrain::MapFormat height = store.format(MapId{MapKind::Height, 0});
    if (height.exists){
        const Vector2 texel = store.texelOfWorld(MapId{MapKind::Height, 0}, infoLocal.x, infoLocal.z);
        const float size = height.bounds.w > 1 ? terrain.terrainSize / static_cast<float>(height.bounds.w - 1) : 0.0f;
        settingsRow("Texel");
        ImGui::Text("%d, %d (%.2f m)", static_cast<int>(std::lround(texel.x)), static_cast<int>(std::lround(texel.y)), size);
        settingsRow("Brush");
        ImGui::Text("%.2f m, %.1f texels", brushSize, size > 0.0f ? brushSize / size : 0.0f);
    }

    // the three heaviest layers, and the base when they do not add to 1
    std::vector<std::pair<float, int>> weights;
    float total = 0.0f;
    std::vector<float> texel(4);
    for (int l = 0; l < static_cast<int>(terrain.surfaceLayers.size()); l++){
        const terrain::LayerChannel channel = terrain::terrainLayerChannel(l);
        const MapId blend{MapKind::Blend, channel.map};
        const terrain::MapFormat format = store.format(blend);
        if (!format.exists) continue;
        const Vector2 at = store.texelOfWorld(blend, infoLocal.x, infoLocal.z);
        const int bx = std::clamp(static_cast<int>(std::lround(at.x)), 0, format.bounds.w - 1);
        const int by = std::clamp(static_cast<int>(std::lround(at.y)), 0, format.bounds.h - 1);
        texel.assign(static_cast<size_t>(format.channels), 0.0f);
        store.read(blend, terrain::TexelRect{bx, by, 1, 1}, texel);
        const float value = channel.inverted ? 1.0f - texel[channel.channel] : texel[channel.channel];
        weights.push_back({value, l});
        total += value;
    }
    std::sort(weights.begin(), weights.end(), [](const auto& a, const auto& b){ return a.first > b.first; });
    for (size_t i = 0; i < weights.size() && i < 3; i++){
        const std::string label = "Layer " + std::to_string(weights[i].second);
        settingsRow(label.c_str());
        ImGui::ProgressBar(weights[i].first, ImVec2(-1.0f, 0.0f));
    }
    if (!weights.empty() && total < 0.999f){
        settingsRow("Base");
        ImGui::ProgressBar(std::max(0.0f, 1.0f - total), ImVec2(-1.0f, 0.0f));
    }
    if (selectedFoliageLayer >= 0 && selectedFoliageLayer < static_cast<int>(terrain.foliageLayers.size())){
        const MapId density{MapKind::Density, selectedFoliageLayer};
        const terrain::MapFormat format = store.format(density);
        if (format.exists){
            const Vector2 at = store.texelOfWorld(density, infoLocal.x, infoLocal.z);
            const int dx = std::clamp(static_cast<int>(std::lround(at.x)), 0, format.bounds.w - 1);
            const int dy = std::clamp(static_cast<int>(std::lround(at.y)), 0, format.bounds.h - 1);
            texel.assign(static_cast<size_t>(format.channels), 0.0f);
            store.read(density, terrain::TexelRect{dx, dy, 1, 1}, texel);
            settingsRow("Details");
            ImGui::ProgressBar(texel[0], ImVec2(-1.0f, 0.0f));
        }
    }
    ImGui::EndTable();
}

void editor::TerrainEditWindow::drawPaintByRules(SceneProject* sceneProject, TerrainComponent& terrain){
    if (!Theme::foldoutHeader("Paint by Rules")) return;
    TerrainEditorSettings& ts = project->getTerrainEditorSettings();
    std::vector<TerrainPaintRule>& rules = ts.paintRules;
    std::vector<std::string> layers = {"Base"};
    for (size_t l = 0; l < terrain.surfaceLayers.size(); l++) layers.push_back("Layer " + std::to_string(l));
    int moveUp = -1, remove = -1;
    for (size_t i = 0; i < rules.size(); i++){
        TerrainPaintRule& rule = rules[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Separator();
        int layer = std::clamp(rule.layer + 1, 0, static_cast<int>(layers.size()) - 1);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        if (ImGui::BeginCombo("##rule_layer", layers[static_cast<size_t>(layer)].c_str())){
            for (int l = 0; l < static_cast<int>(layers.size()); l++){
                if (ImGui::Selectable(layers[static_cast<size_t>(l)].c_str(), l == layer)) rule.layer = l - 1;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(i == 0);
        if (ImGui::SmallButton(ICON_FA_ARROW_UP)) moveUp = static_cast<int>(i);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_TRASH)) remove = static_cast<int>(i);
        ImGui::Checkbox("Slope", &rule.useSlope);
        ImGui::SameLine();
        ImGui::BeginDisabled(!rule.useSlope);
        ImGui::DragFloatRange2("##rule_slope", &rule.slopeMin, &rule.slopeMax, 0.5f, 0.0f, 90.0f, "%.0f deg", "%.0f deg", ImGuiSliderFlags_AlwaysClamp);
        ImGui::EndDisabled();
        ImGui::Checkbox("Height", &rule.useHeight);
        ImGui::SameLine();
        ImGui::BeginDisabled(!rule.useHeight);
        ImGui::DragFloatRange2("##rule_height", &rule.heightMin, &rule.heightMax, 0.005f, 0.0f, 1.0f, "%.2f", "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::EndDisabled();
        ImGui::Checkbox("Invert", &rule.invert);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        ImGui::SliderFloat("Opacity", &rule.opacity, 0.0f, 1.0f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        ImGui::SliderFloat("Noise", &rule.noise, 0.0f, 1.0f, "%.2f");
        ImGui::PopID();
    }
    if (moveUp > 0) std::swap(rules[static_cast<size_t>(moveUp)], rules[static_cast<size_t>(moveUp) - 1]);
    if (remove >= 0) rules.erase(rules.begin() + remove);
    if (ImGui::Button(ICON_FA_PLUS " Add Rule")) rules.push_back(TerrainPaintRule());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
    ImGui::InputInt("Seed", &ts.paintRulesSeed);
    ts.paintRulesSeed = std::max(0, ts.paintRulesSeed);

    ImGui::BeginDisabled(rules.empty() || terrain.heightMap.empty());
    if (ImGui::Button("Apply")){
        terrain::Op op;
        op.type = terrain::OpType::PaintByRules;
        op.seed = static_cast<uint32_t>(ts.paintRulesSeed);
        for (const TerrainPaintRule& rule : rules){
            terrain::PaintRule r;
            r.layer = rule.layer;
            if (rule.useSlope) r.slope = terrain::FilterRange{rule.slopeMin, rule.slopeMax};
            if (rule.useHeight) r.height = terrain::FilterRange{rule.heightMin * terrain.maxHeight, rule.heightMax * terrain.maxHeight};
            r.invert = rule.invert;
            r.opacity = rule.opacity;
            r.noise = rule.noise;
            op.rules.push_back(r);
        }
        const std::vector<terrain::Op> ops = {op};
        terrain::OpOptions options;
        options.newMapResolution = blendMapResolution;
        const terrain::TerrainOpResult preview = previewTerrainOps(project, sceneProject, selectedEntity, ops, options);
        CommandHistory* history = CommandHandle::get(sceneProject->id);
        const size_t start = history->mark();
        terrain::TerrainOpResult reported;
        std::string error;
        if (preview.ok && applyTerrainOpsWithUndo(project, sceneProject, selectedEntity, ops, options, preview, "Paint by Rules", reported, error)){
            history->groupSince(start, "Paint by Rules");
            int64_t texels = 0;
            for (const terrain::MapChangeReport& change : reported.changed) texels += change.texels;
            paintRulesMessage = "Painted " + std::to_string(texels) + " texels.";
        }else{
            while (history->position() > start && history->canUndo() && history->undo()){}
            paintRulesMessage = preview.ok ? error : preview.error;
        }
    }
    ImGui::EndDisabled();
    if (!paintRulesMessage.empty()){
        ImGui::SameLine();
        ImGui::TextDisabled("%s", paintRulesMessage.c_str());
    }
}

template bool editor::TerrainEditWindow::setTerrainProperty<float>(const char*, const float&);
template bool editor::TerrainEditWindow::setTerrainProperty<int>(const char*, const int&);
template bool editor::TerrainEditWindow::setTerrainProperty<bool>(const char*, const bool&);
