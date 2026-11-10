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
#include "command/type/TerrainTextureEditCmd.h"
#include "external/IconsFontAwesome6.h"
#include "math/AABB.h"
#include "object/TerrainEdit.h"
#include "sceneview/SceneViewController.h"
#include "terrain/TerrainIssues.h"
#include "terrain/TerrainLayers.h"
#include "terrain/TerrainOpsUndo.h"
#include "terrain/TerrainOverlay.h"
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
using terraincore::MapId;
using terraincore::MapKind;

namespace {

    // T1 R67: the window's rules as the paint_by_rules op Apply and Preview run
    terraincore::Op paintByRulesOp(const TerrainEditorSettings& ts, const TerrainComponent& terrain) {
        terraincore::Op op;
        op.type = terraincore::OpType::PaintByRules;
        op.seed = static_cast<uint32_t>(ts.paintRulesSeed);
        for (const TerrainPaintRule& rule : ts.paintRules) {
            terraincore::PaintRule r;
            r.layer = rule.layer;
            if (rule.useSlope) r.slope = terraincore::FilterRange{rule.slopeMin, rule.slopeMax};
            if (rule.useHeight) r.height = terraincore::FilterRange{rule.heightMin * terrain.maxHeight, rule.heightMax * terrain.maxHeight};
            r.invert = rule.invert;
            r.opacity = rule.opacity;
            r.noise = rule.noise;
            op.rules.push_back(r);
        }
        return op;
    }

    bool samePaintRules(const std::vector<TerrainPaintRule>& a, const std::vector<TerrainPaintRule>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            const TerrainPaintRule& x = a[i];
            const TerrainPaintRule& y = b[i];
            if (x.layer != y.layer || x.useSlope != y.useSlope || x.slopeMin != y.slopeMin || x.slopeMax != y.slopeMax || x.useHeight != y.useHeight ||
                x.heightMin != y.heightMin || x.heightMax != y.heightMax || x.invert != y.invert || x.opacity != y.opacity || x.noise != y.noise) {
                return false;
            }
        }
        return true;
    }

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
        terraincore::Op remap;
        remap.type = terraincore::OpType::RemapHeights;
        remap.inRange = terraincore::FilterRange{0.0f, height};
        remap.outRange = terraincore::FilterRange{0.0f, before};
        const std::vector<terraincore::Op> ops = {remap};
        const terraincore::TerrainOpResult preview = previewTerrainOps(project, sceneProject, selectedEntity, ops, terraincore::OpOptions());
        terraincore::TerrainOpResult reported;
        if (!preview.ok || !applyTerrainOpsWithUndo(project, sceneProject, selectedEntity, ops, terraincore::OpOptions(), preview, "Terrain Height", reported, error)){
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
            const terraincore::MapFormat format = store.format(map);
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
            ImGui::SameLine();
            ImGui::PushID(name.c_str());
            if (ImGui::SmallButton("Resize...")){
                resizeMap = ResizeMapDialog();
                resizeMap.open = true;
                resizeMap.justOpened = true;
                resizeMap.map = map;
                resizeMap.size = format.bounds.w;
            }
            if (map.kind == MapKind::Blend){
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) deleteMapForTarget(TerrainMapRef(TerrainMapTarget::BlendMap, map.index));
            }
            ImGui::PopID();
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
        const terraincore::MapFormat height = store.format(MapId{MapKind::Height, 0});
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
            const terraincore::MapFormat format = store.format(map);
            if (format.exists) bytes += static_cast<double>(format.bounds.area()) * format.channels * (format.bitsPerChannel / 8);
        }
        for (size_t i = 0; i < terrain.blendMaps.size(); i++){
            const terraincore::MapFormat format = store.format(MapId{MapKind::Blend, static_cast<int>(i)});
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

    // R59: Resize... resamples one map to a new size, one undo step
    if (resizeMap.justOpened){
        ImGui::OpenPopup("Resize Terrain Map");
        resizeMap.justOpened = false;
    }
    if (EditorModal::begin("Resize Terrain Map", &resizeMap.open, ImGuiWindowFlags_AlwaysAutoResize)){
        EditorModal::message("Resample the map to a new size: heights and weights keep their place on the terrain.");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        EditorModal::inputInt("Size", "size", &resizeMap.size, 0, 0);
        resizeMap.size = std::clamp(resizeMap.size, 2, 8192);
        if (EditorModal::button("Resize", "ok")){
            std::string why;
            if (!resizeTerrainMap(project, sceneProject, selectedEntity, TerrainMapUtils::refOfMap(resizeMap.map), resizeMap.size, resizeMap.size, why)){
                Out::error("%s", why.c_str());
            }
            resizeMap.open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (EditorModal::button("Cancel", "cancel")){
            resizeMap.open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

editor::TerrainMapSnapshot editor::TerrainEditWindow::resizedSnapshot(const TerrainMapSnapshot& source, int width, int height, bool holes){
    TerrainMapSnapshot out = source;
    out.width = width;
    out.height = height;
    const int channels = std::max(1, source.channels);
    const int bytes = static_cast<int>(TextureData::getBytesPerChannel(source.colorFormat));
    out.pixels.assign(static_cast<size_t>(width) * height * channels * bytes, 0);
    if (source.width < 1 || source.height < 1 || source.pixels.empty()) return out;
    auto read = [&](int x, int y, int c){
        const size_t at = ((static_cast<size_t>(y) * source.width + x) * channels + c) * bytes;
        return bytes == 2 ? static_cast<float>(source.pixels[at] | (source.pixels[at + 1] << 8)) : static_cast<float>(source.pixels[at]);
    };
    // corner texels sit on the terrain's edges in both sizes
    const float scaleX = width > 1 ? static_cast<float>(source.width - 1) / static_cast<float>(width - 1) : 0.0f;
    const float scaleY = height > 1 ? static_cast<float>(source.height - 1) / static_cast<float>(height - 1) : 0.0f;
    for (int y = 0; y < height; y++){
        const float sy = y * scaleY;
        const int y0 = std::min(static_cast<int>(sy), source.height - 1);
        const int y1 = std::min(y0 + 1, source.height - 1);
        const float fy = sy - static_cast<float>(y0);
        for (int x = 0; x < width; x++){
            const float sx = x * scaleX;
            const int x0 = std::min(static_cast<int>(sx), source.width - 1);
            const int x1 = std::min(x0 + 1, source.width - 1);
            const float fx = sx - static_cast<float>(x0);
            for (int c = 0; c < channels; c++){
                float value;
                if (holes){
                    value = read(fx < 0.5f ? x0 : x1, fy < 0.5f ? y0 : y1, c);
                }else{
                    const float top = read(x0, y0, c) + (read(x1, y0, c) - read(x0, y0, c)) * fx;
                    const float bottom = read(x0, y1, c) + (read(x1, y1, c) - read(x0, y1, c)) * fx;
                    value = top + (bottom - top) * fy;
                }
                const unsigned rounded = static_cast<unsigned>(std::lround(value));
                const size_t at = ((static_cast<size_t>(y) * width + x) * channels + c) * bytes;
                out.pixels[at] = static_cast<unsigned char>(rounded & 0xFF);
                if (bytes == 2) out.pixels[at + 1] = static_cast<unsigned char>(rounded >> 8);
            }
        }
    }
    return out;
}

bool editor::TerrainEditWindow::resizeTerrainMap(Project* project, SceneProject* sceneProject, Entity entity, const TerrainMapRef& ref, int width,
                                                 int height, std::string& error){
    TerrainComponent& terrain = sceneProject->scene->getComponent<TerrainComponent>(entity);
    Texture* texture = TerrainMapUtils::findTexture(terrain, ref);
    if (!texture || texture->empty()){
        error = "The terrain has no such map to resize.";
        return false;
    }
    TerrainMapSnapshot before = TerrainMapUtils::captureSnapshot(project, *texture, true);
    if (before.empty || before.pixels.empty()){
        error = "The map's pixels could not be read.";
        return false;
    }
    TerrainMapSnapshot after = resizedSnapshot(before, width, height, ref.target == TerrainMapTarget::HoleMap);
    after.path = TerrainMapUtils::makeEditableTexturePath(project, sceneProject->id, entity, ref);
    after.id = after.path;
    if (!CommandHandle::get(sceneProject->id)->addCommandNoMerge(
            new TerrainTextureEditCmd(project, sceneProject->id, entity, ref, std::move(before), std::move(after), "Resize Terrain Map"))){
        error = "The map could not be resized.";
        return false;
    }
    return true;
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
    const terraincore::MapFormat format = store.format(MapId{MapKind::Height, 0});
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
    terraincore::Op op;
    op.type = terraincore::OpType::ImportHeightmap;
    op.importedWidth = p.width;
    op.importedHeight = p.height;
    op.importedHeights = p.normalized;
    const std::vector<terraincore::Op> ops = {op};
    const terraincore::TerrainOpResult preview = previewTerrainOps(project, sceneProject, selectedEntity, ops, terraincore::OpOptions());
    terraincore::TerrainOpResult reported;
    if (!preview.ok || !applyTerrainOpsWithUndo(project, sceneProject, selectedEntity, ops, terraincore::OpOptions(), preview, "Import Heightmap", reported, error)){
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
            const terraincore::MapFormat format = store.format(heightMap);
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
    const terraincore::SurfaceSample sample = querySurface(terrain, infoLocal.x, infoLocal.z);
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
    const terraincore::MapFormat height = store.format(MapId{MapKind::Height, 0});
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
        const terraincore::LayerChannel channel = terraincore::terrainLayerChannel(l);
        const MapId blend{MapKind::Blend, channel.map};
        const terraincore::MapFormat format = store.format(blend);
        if (!format.exists) continue;
        const Vector2 at = store.texelOfWorld(blend, infoLocal.x, infoLocal.z);
        const int bx = std::clamp(static_cast<int>(std::lround(at.x)), 0, format.bounds.w - 1);
        const int by = std::clamp(static_cast<int>(std::lround(at.y)), 0, format.bounds.h - 1);
        texel.assign(static_cast<size_t>(format.channels), 0.0f);
        store.read(blend, terraincore::TexelRect{bx, by, 1, 1}, texel);
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
        const terraincore::MapFormat format = store.format(density);
        if (format.exists){
            const Vector2 at = store.texelOfWorld(density, infoLocal.x, infoLocal.z);
            const int dx = std::clamp(static_cast<int>(std::lround(at.x)), 0, format.bounds.w - 1);
            const int dy = std::clamp(static_cast<int>(std::lround(at.y)), 0, format.bounds.h - 1);
            texel.assign(static_cast<size_t>(format.channels), 0.0f);
            store.read(density, terraincore::TexelRect{dx, dy, 1, 1}, texel);
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
    const std::vector<TerrainPaintRule> rulesBefore = rules;
    const int seedBefore = ts.paintRulesSeed;
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
    if (!samePaintRules(rules, rulesBefore) || ts.paintRulesSeed != seedBefore) TerrainOverlay::clearPreview(sceneProject->scene, selectedEntity);

    ImGui::BeginDisabled(rules.empty() || terrain.heightMap.empty());
    // R67: Preview shows the result in the Layer Weights overlay from a run on a copy; any change ends it
    const bool previewing = TerrainOverlay::hasPreview(sceneProject->scene, selectedEntity);
    if (ImGui::Checkbox("Preview", &paintRulesPreview)){
        if (!paintRulesPreview) TerrainOverlay::clearPreview(sceneProject->scene, selectedEntity);
    }
    if (paintRulesPreview && !previewing){
        std::string error;
        if (previewPaintByRules(project, sceneProject, selectedEntity, paintByRulesOp(ts, terrain), blendMapResolution, error)){
            sceneProject->displaySettings.terrainOverlay.source = "layer_weights";
        }else{
            paintRulesMessage = error;
            paintRulesPreview = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply")){
        TerrainOverlay::clearPreview(sceneProject->scene, selectedEntity);
        paintRulesPreview = false;
        const terraincore::Op op = paintByRulesOp(ts, terrain);
        const std::vector<terraincore::Op> ops = {op};
        terraincore::OpOptions options;
        options.newMapResolution = blendMapResolution;
        const terraincore::TerrainOpResult preview = previewTerrainOps(project, sceneProject, selectedEntity, ops, options);
        CommandHistory* history = CommandHandle::get(sceneProject->id);
        const size_t start = history->mark();
        terraincore::TerrainOpResult reported;
        std::string error;
        if (preview.ok && applyTerrainOpsWithUndo(project, sceneProject, selectedEntity, ops, options, preview, "Paint by Rules", reported, error)){
            history->groupSince(start, "Paint by Rules");
            int64_t texels = 0;
            for (const terraincore::MapChangeReport& change : reported.changed) texels += change.texels;
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

bool editor::TerrainEditWindow::previewPaintByRules(Project* project, SceneProject* sceneProject, Entity entity, const terraincore::Op& op,
                                                     int newMapResolution, std::string& error){
    std::shared_ptr<TerrainOpsSnapshot> snapshot = snapshotTerrainMaps(project, sceneProject, entity);
    if (!snapshot || !snapshot->overlay){
        error = "The terrain's maps could not be copied.";
        return false;
    }
    terraincore::OpOptions options;
    options.newMapResolution = newMapResolution;
    const terraincore::TerrainOpResult result = terraincore::applyOps(*snapshot->overlay, std::span<const terraincore::Op>(&op, 1), options);
    if (!result.ok){
        error = result.error;
        return false;
    }
    // the overlay keeps the copy alive as long as it shows it
    TerrainOverlay::setPreview(sceneProject->scene, entity, std::shared_ptr<const terraincore::TerrainMapStore>(snapshot, snapshot->overlay.get()));
    return true;
}

template bool editor::TerrainEditWindow::setTerrainProperty<float>(const char*, const float&);
template bool editor::TerrainEditWindow::setTerrainProperty<int>(const char*, const int&);
template bool editor::TerrainEditWindow::setTerrainProperty<bool>(const char*, const bool&);
