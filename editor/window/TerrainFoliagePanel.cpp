// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "window/TerrainFoliagePanel.h"

#include "Backend.h"
#include "component/MeshComponent.h"
#include "App.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/PropertyCmd.h"
#include "command/type/TreeEditCmd.h"
#include "component/InstancedMeshComponent.h"
#include "component/ModelComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "foliage/FoliageOpRunner.h"
#include "foliage/TerrainOverlay.h"
#include "foliage/TreeDataFile.h"
#include "subsystem/FoliageSystem.h"
#include "terrain/FoliageRules.h"
#include "terrain/FoliageScatter.h"
#include "util/ImpostorBaker.h"
#include "util/Util.h"
#include "window/ProjectWindow.h"
#include "window/TerrainEditWindow.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::terrain;
namespace fs = std::filesystem;

namespace {

    const char* const FOLIAGE_TIER_NAMES[] = {"Desktop", "Mobile", "Web"};

    void rowLabel(const char* label, const char* tooltip = nullptr) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
    }

    double foliageSeconds() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::string prototypeName(const TerrainTreePrototype& prototype) {
        return prototype.meshPath.empty() ? std::string("Empty") : fs::path(prototype.meshPath.str()).stem().string();
    }

}

editor::TerrainFoliagePanel::TerrainFoliagePanel(Project* project, TerrainEditWindow* window): project(project), window(window) {}

editor::TerrainFoliagePanel::~TerrainFoliagePanel() = default;

bool editor::TerrainFoliagePanel::isReady(const TerrainComponent& terrain) const {
    return prototype >= 0 && prototype < static_cast<int>(terrain.treePrototypes.size());
}

bool editor::TerrainFoliagePanel::setPrototypes(SceneProject* sceneProject, Entity entity, const std::vector<TerrainTreePrototype>& prototypes, const std::string& name) {
    (void)name;
    CommandHandle::get(sceneProject->id)->addCommandNoMerge(new PropertyCmd<std::vector<TerrainTreePrototype>>(
        project, sceneProject->id, entity, ComponentType::TerrainComponent, "treePrototypes", prototypes));
    return true;
}

void editor::TerrainFoliagePanel::addPrototype(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain, const std::string& meshPath) {
    if (terrain.treePrototypes.size() >= static_cast<size_t>(FOLIAGE_MAX_PROTOTYPES)) return;
    std::vector<TerrainTreePrototype> prototypes = terrain.treePrototypes;
    TerrainTreePrototype added;
    uint32_t id = 1;
    for (const TerrainTreePrototype& existing : prototypes) id = std::max(id, existing.id + 1);
    added.id = id;
    added.meshPath = AssetRef(meshPath);
    prototypes.push_back(added);
    if (setPrototypes(sceneProject, entity, prototypes, "Add Tree Prototype")) {
        prototype = static_cast<int>(prototypes.size()) - 1;
    }
}

void editor::TerrainFoliagePanel::removePrototype(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain, int index) {
    if (index < 0 || index >= static_cast<int>(terrain.treePrototypes.size())) return;
    FoliageSystem* foliage = sceneProject->scene->getSystem<FoliageSystem>().get();
    const uint32_t id = terrain.treePrototypes[index].id;

    // one undo step: the records go with their prototype
    CommandHistory* history = CommandHandle::get(sceneProject->id);
    const size_t start = history->mark();
    if (std::shared_ptr<TreeStore> store = foliage->getTreeStore(entity)) {
        std::vector<uint32_t> removed;
        for (const TreeRecord& record : store->records()) {
            if (record.prototypeId == id) removed.push_back(record.id);
        }
        if (!removed.empty()) {
            TreeDelta delta;
            store->apply(removed, {}, {}, &delta);
            history->addCommand(new TreeEditCmd(project, sceneProject->id, entity, "Erase Trees (" + sceneProject->scene->getEntityName(entity) + ")", std::move(delta)));
        }
    }
    std::vector<TerrainTreePrototype> prototypes = terrain.treePrototypes;
    prototypes.erase(prototypes.begin() + index);
    setPrototypes(sceneProject, entity, prototypes, "Remove Tree Prototype");
    history->groupSince(start, "Remove Tree Prototype");
    prototype = std::clamp(prototype, 0, std::max(0, static_cast<int>(prototypes.size()) - 1));
}

void editor::TerrainFoliagePanel::drawPrototypeGrid(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain) {
    const float cell = ImGui::GetFrameHeight() * 3.0f;
    const float available = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, static_cast<int>(available / (cell + ImGui::GetStyle().ItemSpacing.x)));
    FoliageSystem* foliage = sceneProject->scene->getSystem<FoliageSystem>().get();

    static int removeIndex = -1;
    ImGui::PushID("tree_prototypes");
    for (int i = 0; i < static_cast<int>(terrain.treePrototypes.size()); i++) {
        ImGui::PushID(i);
        if (i % columns != 0) ImGui::SameLine();
        const TerrainTreePrototype& authored = terrain.treePrototypes[i];
        int width = 0, height = 0;
        ImTextureID thumbnail = Backend::getApp().getProjectWindow()->getAssetThumbnail(authored.meshPath.str(), width, height);
        const bool selected = i == prototype;
        if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
        const std::string name = prototypeName(authored);
        bool clicked;
        if (thumbnail) {
            clicked = ImGui::ImageButton("##thumb", thumbnail, ImVec2(cell, cell));
        } else {
            clicked = ImGui::Button(name.c_str(), ImVec2(cell, cell));
        }
        if (selected) ImGui::PopStyleColor();
        if (clicked) prototype = i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", name.c_str());
        if (ImGui::BeginPopupContextItem("##context")) {
            if (ImGui::MenuItem("Edit")) prototype = i;
            if (ImGui::MenuItem("Bake Impostor")) {
                prototype = i;
                ImpostorBaker::Request request;
                request.sceneId = sceneProject->id;
                request.terrain = entity;
                request.prototypes = {i};
                request.views = bakeViews;
                request.resolution = bakeResolution;
                std::string error;
                Project* owner = project;
                const uint32_t sceneId = sceneProject->id;
                ImpostorBaker::start(project, request, [owner, sceneId, entity](const std::vector<ImpostorBakeResult>& results, bool cancelled) {
                    if (!cancelled) ImpostorBaker::applyResults(owner, sceneId, entity, results);
                }, nullptr, error);
                if (!error.empty()) bakeStatus = error;
            }
            if (ImGui::MenuItem("Remove")) removeIndex = i;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::PopID();

    // removing asks when instances exist
    if (removeIndex >= 0) {
        uint64_t count = 0;
        if (removeIndex < static_cast<int>(terrain.treePrototypes.size())) {
            if (std::shared_ptr<TreeStore> store = foliage->getTreeStore(entity)) {
                for (const TreeRecord& record : store->records()) {
                    if (record.prototypeId == terrain.treePrototypes[removeIndex].id) count++;
                }
            }
        }
        if (count == 0) {
            removePrototype(sceneProject, entity, terrain, removeIndex);
            removeIndex = -1;
        } else {
            ImGui::OpenPopup("Remove Tree Prototype");
        }
    }
    if (ImGui::BeginPopupModal("Remove Tree Prototype", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("This prototype has trees on the Terrain. Remove them with it?");
        if (ImGui::Button("Remove")) {
            removePrototype(sceneProject, entity, terrain, removeIndex);
            removeIndex = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            removeIndex = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::Button("Add##tree_prototype")) ImGui::OpenPopup("##tree_add");
    if (ImGui::BeginPopup("##tree_add")) {
        static char path[512] = "";
        ImGui::InputText("Model", path, sizeof(path));
        if (ImGui::Button("Add") && path[0]) {
            addPrototype(sceneProject, entity, terrain, path);
            path[0] = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Drop a model here");
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
            const std::string dropped(static_cast<const char*>(payload->Data), static_cast<size_t>(payload->DataSize));
            addPrototype(sceneProject, entity, terrain, project->normalizeToAssetsRelative(dropped).generic_string());
        }
        ImGui::EndDragDropTarget();
    }
}

void editor::TerrainFoliagePanel::drawImpostor(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain, TerrainTreePrototype& authored) {
    rowLabel("Impostor", "The last LOD: one quad per instance, lit through baked normals. Baked per mesh, one view per frame.");
    const ImpostorState state = ImpostorBaker::stateOf(project, authored.meshPath.str(), authored.impostorViews, authored.impostorSourceHash);
    ImGui::TextUnformatted(state == ImpostorState::None ? "None" : state == ImpostorState::Baked ? "Baked" : "Out of Date");

    rowLabel("Views");
    int viewChoice = bakeViews == 4 ? 0 : bakeViews == 16 ? 2 : 1;
    const char* const viewNames[] = {"4", "8", "16"};
    if (ImGui::Combo("##impostor_views", &viewChoice, viewNames, 3)) bakeViews = viewChoice == 0 ? 4 : viewChoice == 2 ? 16 : 8;
    rowLabel("Resolution");
    int resolutionChoice = bakeResolution == 128 ? 0 : bakeResolution == 512 ? 2 : 1;
    const char* const resolutionNames[] = {"128", "256", "512"};
    if (ImGui::Combo("##impostor_resolution", &resolutionChoice, resolutionNames, 3)) bakeResolution = resolutionChoice == 0 ? 128 : resolutionChoice == 2 ? 512 : 256;

    rowLabel("Bake");
    const bool baking = ImpostorBaker::isRunning();
    ImGui::BeginDisabled(baking);
    const auto startBake = [&](std::vector<int> prototypes) {
        ImpostorBaker::Request request;
        request.sceneId = sceneProject->id;
        request.terrain = entity;
        request.prototypes = std::move(prototypes);
        request.views = bakeViews;
        request.resolution = bakeResolution;
        std::string error;
        Project* owner = project;
        const uint32_t sceneId = sceneProject->id;
        double* share = &bakeShare;
        bakeShare = 0.0;
        const bool started = ImpostorBaker::start(project, request, [owner, sceneId, entity, share](const std::vector<ImpostorBakeResult>& results, bool cancelled) {
            *share = -1.0;
            if (!cancelled) ImpostorBaker::applyResults(owner, sceneId, entity, results);
        }, [share](double done) { *share = done; }, error);
        bakeStatus = started ? std::string() : error;
        if (!started) bakeShare = -1.0;
    };
    if (ImGui::Button("Bake Impostor")) startBake({prototype});
    ImGui::SameLine();
    if (ImGui::Button("Bake All")) {
        std::vector<int> all;
        for (int i = 0; i < static_cast<int>(terrain.treePrototypes.size()); i++) all.push_back(i);
        startBake(std::move(all));
    }
    ImGui::EndDisabled();
    if (baking) {
        ImGui::ProgressBar(static_cast<float>(std::max(bakeShare, 0.0)), ImVec2(-FLT_MIN, 0.0f));
        if (ImGui::Button("Cancel##bake")) ImpostorBaker::cancel();
    }
    if (!bakeStatus.empty()) ImGui::TextWrapped("%s", bakeStatus.c_str());

    if (!authored.impostorAlbedo.empty()) {
        int width = 0, height = 0;
        ImTextureID atlas = Backend::getApp().getProjectWindow()->getAssetThumbnail(authored.impostorAlbedo.getPath(0), width, height);
        if (atlas) {
            rowLabel("Atlas");
            ImGui::Image(atlas, ImVec2(ImGui::GetFrameHeight() * 6.0f, ImGui::GetFrameHeight() * 3.0f));
        }
    }
}

void editor::TerrainFoliagePanel::drawPrototypeFoldout(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain) {
    if (!isReady(terrain)) return;
    if (!ImGui::CollapsingHeader("Selected Prototype", ImGuiTreeNodeFlags_DefaultOpen)) return;

    TerrainTreePrototype authored = terrain.treePrototypes[prototype];
    bool changed = false;
    if (!ImGui::BeginTable("##prototype_rows", 2, ImGuiTableFlags_SizingStretchProp)) return;

    rowLabel("LODs", "Explicit LOD meshes and where they start; none: the generated levels of the mesh.");
    ImGui::TextUnformatted(authored.lodMeshes.empty() ? "Generated" : "Explicit");
    for (size_t l = 0; l < authored.lodMeshes.size(); l++) {
        rowLabel(("LOD " + std::to_string(l + 1)).c_str());
        ImGui::TextUnformatted(authored.lodMeshes[l].str().c_str());
        if (l < authored.lodDistances.size()) {
            rowLabel(("LOD " + std::to_string(l + 1) + " Distance").c_str());
            changed |= ImGui::DragFloat(("##lod_distance" + std::to_string(l)).c_str(), &authored.lodDistances[l], 1.0f, 0.0f, 5000.0f, "%.0f");
        }
    }

    rowLabel("Bend Factor", "How far the wind moves the whole tree, height squared; 0 draws the variant without wind.");
    changed |= ImGui::SliderFloat("##bend_factor", &authored.bendFactor, 0.0f, 1.0f, "%.2f");
    rowLabel("Wind Mask", "What masks the leaves' flutter.");
    int windMask = static_cast<int>(authored.windMask);
    const char* const maskNames[] = {"Height", "Vertex Color"};
    if (ImGui::Combo("##wind_mask", &windMask, maskNames, 2)) {
        authored.windMask = static_cast<TerrainTreeWindMask>(windMask);
        changed = true;
    }

    rowLabel("Shadows", "Which bands cast: each mesh LOD, and the impostor.");
    for (int l = 0; l < 4; l++) {
        if (l > 0) ImGui::SameLine();
        bool on = (authored.shadowLodMask & (1u << l)) != 0;
        if (ImGui::Checkbox(("##shadow_lod" + std::to_string(l)).c_str(), &on)) {
            authored.shadowLodMask = static_cast<uint8_t>(on ? authored.shadowLodMask | (1u << l) : authored.shadowLodMask & ~(1u << l));
            changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("LOD %d casts", l);
    }
    ImGui::SameLine();
    bool impostorCasts = (authored.shadowLodMask & 0x80u) != 0;
    if (ImGui::Checkbox("##shadow_impostor", &impostorCasts)) {
        authored.shadowLodMask = static_cast<uint8_t>(impostorCasts ? authored.shadowLodMask | 0x80u : authored.shadowLodMask & 0x7Fu);
        changed = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The impostor casts");

    rowLabel("Collision", "A static capsule per tree, while the scene simulates.");
    int collision = static_cast<int>(authored.collision);
    const char* const collisionNames[] = {"None", "Capsule"};
    if (ImGui::Combo("##collision", &collision, collisionNames, 2)) {
        authored.collision = static_cast<TerrainTreeCollision>(collision);
        changed = true;
    }
    if (authored.collision == TerrainTreeCollision::Capsule) {
        rowLabel("Radius");
        changed |= ImGui::DragFloat("##collision_radius", &authored.collisionRadius, 0.01f, 0.01f, 50.0f, "%.2f");
        rowLabel("Height");
        changed |= ImGui::DragFloat("##collision_height", &authored.collisionHeight, 0.05f, 0.01f, 500.0f, "%.2f");
        rowLabel("Fit");
        if (ImGui::Button("Fit from Mesh")) {
            FoliageSystem* foliage = sceneProject->scene->getSystem<FoliageSystem>().get();
            const Entity meshEntity = foliage->getPrototypeEntity(entity, prototype);
            if (const MeshComponent* mesh = meshEntity != NULL_ENTITY ? sceneProject->scene->findComponent<MeshComponent>(meshEntity) : nullptr) {
                if (!mesh->verticesAABB.isNull()) {
                    const Vector3 half = mesh->verticesAABB.getHalfSize();
                    // the trunk: a share of the crown's reach, the whole height
                    authored.collisionRadius = std::max(std::min(half.x, half.z) * 0.25f, 0.05f);
                    authored.collisionHeight = half.y * 2.0f;
                    changed = true;
                }
            }
        }
    }

    rowLabel("Nav Obstacle", "Baked navmeshes go around these trees.");
    changed |= ImGui::Checkbox("##nav_obstacle", &authored.navObstacle);

    if (changed) {
        CommandHandle::get(sceneProject->id)->addCommand(new PropertyCmd<TerrainTreePrototype>(
            project, sceneProject->id, entity, ComponentType::TerrainComponent, "treePrototypes[" + std::to_string(prototype) + "]", authored));
    }

    drawImpostor(sceneProject, entity, terrain, authored);
    ImGui::EndTable();
}

void editor::TerrainFoliagePanel::drawMassPlace(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain) {
    if (ImGui::Button("Mass Place Trees...")) {
        massOpen = true;
        ImGui::OpenPopup("Mass Place Trees");
    }
    if (!ImGui::BeginPopupModal("Mass Place Trees", &massOpen, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::DragInt("Number of Trees", &massCount, 10.0f, 0, 2000000);
    ImGui::Checkbox("Keep Existing Trees", &massKeepExisting);
    ImGui::DragInt("Seed", &massSeed, 1.0f, 0, INT32_MAX);
    massWeights.resize(terrain.treePrototypes.size(), 1.0f);
    for (size_t i = 0; i < terrain.treePrototypes.size(); i++) {
        ImGui::DragFloat((prototypeName(terrain.treePrototypes[i]) + "##weight" + std::to_string(i)).c_str(), &massWeights[i], 0.01f, 0.0f, 100.0f, "%.2f");
    }
    ImGui::DragFloat2("Slope", massSlope, 0.5f, 0.0f, 90.0f, "%.0f deg");
    ImGui::DragFloat2("Height", massHeight, 1.0f, -100000.0f, 100000.0f, "%.0f");
    ImGui::Checkbox("Exclude Holes", &massExcludeHoles);

    const auto makeRequest = [&](bool dryRun) {
        FoliageOpRunner::Request request;
        request.sceneId = sceneProject->id;
        request.terrain = entity;
        request.dryRun = dryRun;
        request.undoName = "Mass Place Trees (" + sceneProject->scene->getEntityName(entity) + ")";
        Op op;
        op.type = OpType::Registered;
        op.registeredName = "scatter_by_rules";
        op.seed = static_cast<uint32_t>(massSeed);
        const auto number = [](double value) { OpParam param; param.kind = OpParam::Kind::Number; param.number = value; return param; };
        const auto text = [](const std::string& value) { OpParam param; param.kind = OpParam::Kind::String; param.text = value; return param; };
        OpParam targets;
        targets.kind = OpParam::Kind::List;
        for (size_t i = 0; i < terrain.treePrototypes.size(); i++) {
            OpParam entry;
            entry.kind = OpParam::Kind::Object;
            entry.fields.emplace_back("prototype", number(static_cast<double>(i)));
            entry.fields.emplace_back("weight", number(massWeights[i]));
            targets.items.push_back(std::move(entry));
        }
        OpParam target;
        target.kind = OpParam::Kind::Object;
        target.fields.emplace_back("trees", std::move(targets));
        OpParam range;
        range.kind = OpParam::Kind::List;
        range.items = {number(massSlope[0]), number(massSlope[1])};
        OpParam rangeHeight;
        rangeHeight.kind = OpParam::Kind::List;
        rangeHeight.items = {number(massHeight[0]), number(massHeight[1])};
        OpParam exclude;
        exclude.kind = OpParam::Kind::Object;
        OpParam holes;
        holes.kind = OpParam::Kind::Bool;
        holes.boolean = massExcludeHoles;
        exclude.fields.emplace_back("holes", std::move(holes));
        OpParam rules;
        rules.kind = OpParam::Kind::Object;
        rules.fields.emplace_back("slope", std::move(range));
        rules.fields.emplace_back("height", std::move(rangeHeight));
        rules.fields.emplace_back("exclude", std::move(exclude));
        op.params.kind = OpParam::Kind::Object;
        op.params.fields.emplace_back("target", std::move(target));
        op.params.fields.emplace_back("count", number(massCount));
        op.params.fields.emplace_back("mode", text(massKeepExisting ? "add" : "replace"));
        op.params.fields.emplace_back("rules", std::move(rules));
        request.ops.push_back(std::move(op));
        return request;
    };

    const bool running = massRunner != nullptr;
    ImGui::BeginDisabled(running || terrain.treePrototypes.empty());
    if (ImGui::Button("Preview Count")) {
        std::string* status = &massStatus;
        massStatus = "Counting...";
        massRunner = FoliageOpRunner::start(project, makeRequest(true), [status](const FoliageOpRunner::Outcome& outcome) {
            int64_t added = 0;
            for (const OpEntry& entry : outcome.result.ops) {
                for (const auto& [key, value] : entry.fields) {
                    if (key == "instances_added") added += static_cast<int64_t>(value.number);
                }
            }
            *status = outcome.error.empty() ? std::to_string(added) + " trees would be placed." : outcome.error;
        });
    }
    ImGui::SameLine();
    if (ImGui::Button("Place")) {
        std::string* status = &massStatus;
        massStatus = "Placing...";
        massRunner = FoliageOpRunner::start(project, makeRequest(false), [status](const FoliageOpRunner::Outcome& outcome) {
            *status = !outcome.error.empty() ? outcome.error : outcome.cancelled ? std::string("Cancelled.") : std::to_string(outcome.treesAfter) + " trees on the Terrain.";
        });
    }
    ImGui::EndDisabled();
    if (running) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel##mass")) massRunner->cancel();
        ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-FLT_MIN, 0.0f));
    }
    if (!massStatus.empty()) ImGui::TextWrapped("%s", massStatus.c_str());
    // the job goes on without the dialog: its end lands in the panel's update
    if (ImGui::Button("Close##mass")) {
        massOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

editor::TerrainFoliagePanel::Estimate editor::TerrainFoliagePanel::estimateFor(int tier, const TerrainComponent& terrain, uint64_t detailVisibleHigh, uint64_t treesVisible, uint64_t treesStored, uint32_t draws) {
    const FoliageTierLimits limits = foliageTierLimits(tier == 0 ? FoliageTier::High : FoliageTier::Low);
    const float distanceScale = tier == 0 ? 1.0f : 0.5f;
    Estimate estimate;
    // what a tier leaves of what High shows here: density scales it, Foliage Distance by its square
    estimate.detailVisible = static_cast<uint64_t>(static_cast<double>(detailVisibleHigh) * limits.detailDensity * distanceScale * distanceScale);
    estimate.treesVisible = std::min<uint64_t>(treesVisible, limits.maxVisibleTrees);
    estimate.meshTrees = std::min<uint64_t>(treesVisible, std::min<uint64_t>(static_cast<uint64_t>(std::max(terrain.maxMeshTrees, 0)), limits.maxMeshTreesCap));
    estimate.draws = std::min(draws, limits.maxDraws);
    const uint64_t instanceBytes = (estimate.detailVisible + estimate.treesVisible) * sizeof(FoliageInstance);
    estimate.cpuBytes = treesStored * sizeof(TreeRecord) + instanceBytes;
    estimate.gpuBytes = instanceBytes + estimate.meshTrees * sizeof(FoliageInstance);
    estimate.over = detailVisibleHigh * limits.detailDensity > limits.maxDetailInstances || treesVisible > limits.maxVisibleTrees || draws > limits.maxDraws;
    return estimate;
}

void editor::TerrainFoliagePanel::drawStats(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain) {
    if (!ImGui::CollapsingHeader("Stats")) return;
    FoliageSystem* foliage = sceneProject->scene->getSystem<FoliageSystem>().get();
    const FoliageStats& stats = foliage->getStats(entity);

    const double now = foliageSeconds();
    if (now >= nextStats) {
        nextStats = now + 0.5;
        shownTotal = stats.treesTotal + stats.detailResident;
        shownResident = stats.treesResident + stats.detailResident;
        shownVisible = stats.treesVisible + stats.detailVisible;
        shownCells = stats.cells;
        shownPending = stats.pendingCells;
        shown = estimateFor(estimateTier, terrain, stats.detailVisible, stats.treesVisible, stats.treesTotal, stats.draws);
    }

    ImGui::Combo("Estimate For", &estimateTier, FOLIAGE_TIER_NAMES, 3);
    ImGui::Text("Instances: %llu total, %llu resident, %llu visible", static_cast<unsigned long long>(shownTotal), static_cast<unsigned long long>(shownResident), static_cast<unsigned long long>(shownVisible));
    ImGui::Text("Cells: %u, pending %u", shownCells, shownPending);
    const ImVec4 warning = ImGui::GetStyleColorVec4(ImGuiCol_PlotHistogram);
    const ImVec4 normal = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImGui::TextColored(shown.over ? warning : normal, "%s: %llu details, %llu trees (%llu mesh), %u draws", FOLIAGE_TIER_NAMES[estimateTier],
                       static_cast<unsigned long long>(shown.detailVisible), static_cast<unsigned long long>(shown.treesVisible),
                       static_cast<unsigned long long>(shown.meshTrees), shown.draws);
    ImGui::Text("Estimated memory: CPU %.1f MB, GPU %.1f MB", static_cast<double>(shown.cpuBytes) / (1024.0 * 1024.0), static_cast<double>(shown.gpuBytes) / (1024.0 * 1024.0));
}

void editor::TerrainFoliagePanel::drawTerrainSettings(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain) {
    if (!ImGui::CollapsingHeader("Terrain Settings: Foliage")) return;
    const auto setFloat = [&](const char* field, float value) {
        CommandHandle::get(sceneProject->id)->addCommand(new PropertyCmd<float>(project, sceneProject->id, entity, ComponentType::TerrainComponent, field, value));
    };
    if (!ImGui::BeginTable("##foliage_settings", 2, ImGuiTableFlags_SizingStretchProp)) return;
    float value = terrain.detailDistance;
    rowLabel("Detail Distance", "A Terrain-wide limit on every layer's Draw Distance; 0: none.");
    if (ImGui::DragFloat("##detail_distance", &value, 1.0f, 0.0f, 5000.0f, "%.0f")) setFloat("detailDistance", value);
    value = terrain.detailDensity;
    rowLabel("Detail Density");
    if (ImGui::SliderFloat("##detail_density", &value, 0.0f, 1.0f, "%.2f")) setFloat("detailDensity", value);
    value = terrain.treeDistance;
    rowLabel("Tree Distance");
    if (ImGui::DragFloat("##tree_distance", &value, 1.0f, 0.0f, 20000.0f, "%.0f")) setFloat("treeDistance", value);
    value = terrain.billboardStart;
    rowLabel("Billboard Start", "Where meshes hand over to impostors.");
    if (ImGui::DragFloat("##billboard_start", &value, 1.0f, 0.0f, 20000.0f, "%.0f")) setFloat("billboardStart", value);
    value = terrain.fadeLength;
    rowLabel("Fade Length", "Dither between mesh and impostor.");
    if (ImGui::DragFloat("##fade_length", &value, 0.1f, 0.0f, 200.0f, "%.1f")) setFloat("fadeLength", value);
    int meshTrees = terrain.maxMeshTrees;
    rowLabel("Max Mesh Trees", "Past this, by distance, a cell draws impostors only.");
    if (ImGui::DragInt("##max_mesh_trees", &meshTrees, 1.0f, 0, 100000)) {
        CommandHandle::get(sceneProject->id)->addCommand(new PropertyCmd<int>(project, sceneProject->id, entity, ComponentType::TerrainComponent, "maxMeshTrees", meshTrees));
    }
    Vector3 direction = terrain.windDirection;
    rowLabel("Wind Direction");
    if (ImGui::DragFloat3("##wind_direction", &direction.x, 0.01f, -1.0f, 1.0f, "%.2f")) {
        CommandHandle::get(sceneProject->id)->addCommand(new PropertyCmd<Vector3>(project, sceneProject->id, entity, ComponentType::TerrainComponent, "windDirection", direction));
    }
    value = terrain.windSpeed;
    rowLabel("Wind Speed");
    if (ImGui::DragFloat("##wind_speed", &value, 0.01f, 0.0f, 20.0f, "%.2f")) setFloat("windSpeed", value);
    value = terrain.windSize;
    rowLabel("Wind Size", "World units between waves.");
    if (ImGui::DragFloat("##wind_size", &value, 0.1f, 0.01f, 500.0f, "%.1f")) setFloat("windSize", value);
    value = terrain.windBending;
    rowLabel("Wind Bending");
    if (ImGui::SliderFloat("##wind_bending", &value, 0.0f, 2.0f, "%.2f")) setFloat("windBending", value);
    ImGui::EndTable();
}

void editor::TerrainFoliagePanel::drawWarnings(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain) {
    FoliageSystem* foliage = sceneProject->scene->getSystem<FoliageSystem>().get();
    const FoliageStats& stats = foliage->getStats(entity);
    const ImVec4 warning = ImGui::GetStyleColorVec4(ImGuiCol_PlotHistogram);

    for (int i = 0; i < static_cast<int>(terrain.treePrototypes.size()); i++) {
        const TerrainTreePrototype& authored = terrain.treePrototypes[i];
        if (authored.impostorViews == 0 && terrain.treeDistance > terrain.billboardStart) {
            ImGui::TextColored(warning, "%s has no impostor but draws past Billboard Start: it pays a draw per cell all the way.", prototypeName(authored).c_str());
        }
        const Entity meshEntity = foliage->getPrototypeEntity(entity, i);
        if (const MeshComponent* mesh = meshEntity != NULL_ENTITY ? sceneProject->scene->findComponent<MeshComponent>(meshEntity) : nullptr) {
            if (mesh->loaded && mesh->numSubmeshes == 0) {
                ImGui::TextColored(warning, "%s has no geometry on its root and cannot be instanced.", prototypeName(authored).c_str());
            }
        }
    }
    const FoliageTierLimits low = foliageTierLimits(FoliageTier::Low);
    if (stats.treesTotal > low.maxVisibleTrees) {
        ImGui::TextColored(warning, "%llu trees: past the Mobile budget of %u visible.", static_cast<unsigned long long>(stats.treesTotal), low.maxVisibleTrees);
    }
    std::string refusal;
    if (foliage->isTreeStoreRefused(entity, &refusal)) {
        ImGui::TextColored(warning, "The tree file was refused (%s). It draws no trees and will not be overwritten.", refusal.c_str());
        if (ImGui::Button("Overwrite on Next Save")) foliage->allowOverwriteRefusedStore(entity);
    }
}

void editor::TerrainFoliagePanel::drawConvert(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain) {
    FoliageOpHost host;
    FoliageOpRunner::fillHost(project, sceneProject->id, entity, host);
    for (const auto& [key, placed] : host.placedHosts) {
        if (placed.instances.empty()) continue;
        ImGui::PushID(key.c_str());
        // a host needs a prototype of its model to become trees
        std::string label = "Convert Placed Instances to Trees (" + placed.name + ")";
        bool hasPrototype = false;
        for (const FoliageOpHost::Prototype& prototypeEntry : host.prototypes) hasPrototype = hasPrototype || prototypeEntry.id == placed.prototypeId;
        ImGui::BeginDisabled(!hasPrototype || terrain.treePrototypes.empty());
        if (ImGui::Button(label.c_str())) {
            FoliageOpRunner::Request request;
            request.sceneId = sceneProject->id;
            request.terrain = entity;
            request.undoName = "Convert Placed Instances (" + sceneProject->scene->getEntityName(entity) + ")";
            Op op;
            op.type = OpType::Registered;
            op.registeredName = "convert_placed_to_trees";
            op.params.kind = OpParam::Kind::Object;
            OpParam hostParam;
            hostParam.kind = OpParam::Kind::String;
            hostParam.text = key;
            op.params.fields.emplace_back("host", std::move(hostParam));
            request.ops.push_back(std::move(op));
            FoliageOpRunner::start(project, std::move(request), nullptr);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
}

bool editor::TerrainFoliagePanel::drawTrees(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain, float& brushSize) {
    update();
    drawPrototypeGrid(sceneProject, entity, terrain);

    if (ImGui::BeginTable("##tree_rows", 2, ImGuiTableFlags_SizingStretchProp)) {
        rowLabel("Place As", "Tree: stored in the Terrain's tree data, with LODs, impostors and capsules. GameObject: a child of the Terrain, with its components.");
        int placeAs = placeAsTree ? 0 : 1;
        const char* const placeNames[] = {"Tree", "GameObject"};
        if (ImGui::Combo("##place_as", &placeAs, placeNames, 2)) placeAsTree = placeAs == 0;
        if (placeAsTree) {
            rowLabel("Brush Size");
            ImGui::DragFloat("##tree_brush_size", &brushSize, 0.1f, 0.1f, 500.0f, "%.1f");
            rowLabel("Tree Density", "Trees per square world unit under the brush.");
            ImGui::DragFloat("##tree_density", &treeDensity, 0.01f, 0.001f, 10.0f, "%.3f");
            rowLabel("Tree Height", "Scale of the mesh's height, with a random range.");
            ImGui::DragFloat("##tree_height", &treeHeight, 0.01f, 0.05f, 20.0f, "%.2f");
            rowLabel("Height Random");
            ImGui::SliderFloat("##tree_height_random", &treeHeightRandom, 0.0f, 1.0f, "%.2f");
            rowLabel("Lock Width to Height");
            ImGui::Checkbox("##tree_lock_width", &lockWidthToHeight);
            ImGui::BeginDisabled(lockWidthToHeight);
            rowLabel("Tree Width");
            ImGui::DragFloat("##tree_width", &treeWidth, 0.01f, 0.05f, 20.0f, "%.2f");
            rowLabel("Width Random");
            ImGui::SliderFloat("##tree_width_random", &treeWidthRandom, 0.0f, 1.0f, "%.2f");
            ImGui::EndDisabled();
            rowLabel("Random Tree Rotation");
            ImGui::Checkbox("##tree_rotation", &randomRotation);
            rowLabel("Color Variation");
            ImGui::SliderFloat("##tree_color", &colorVariation, 0.0f, 1.0f, "%.2f");
            rowLabel("Normal Alignment");
            ImGui::SliderFloat("##tree_align", &normalAlignment, 0.0f, 1.0f, "%.2f");
            rowLabel("Spacing", "Closest two trees of a stroke may get, world units.");
            ImGui::DragFloat("##tree_spacing", &spacing, 0.05f, 0.0f, 100.0f, "%.2f");
        }
        ImGui::EndTable();
    }

    if (placeAsTree) {
        drawPrototypeFoldout(sceneProject, entity, terrain);
        drawMassPlace(sceneProject, entity, terrain);
    }
    drawConvert(sceneProject, entity, terrain);

    if (ImGui::Checkbox("Show Tree Density", &showDensity)) {
        TerrainOverlay::show(sceneProject, entity, showDensity ? TerrainOverlay::Kind::TreeDensity : TerrainOverlay::Kind::None, 0, 0.7f);
    }
    drawTerrainSettings(sceneProject, entity, terrain);
    drawStats(sceneProject, entity, terrain);
    drawWarnings(sceneProject, entity, terrain);
    return placeAsTree;
}

void editor::TerrainFoliagePanel::drawDetails(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain, int layerIndex) {
    if (layerIndex >= 0 && layerIndex < static_cast<int>(terrain.foliageLayers.size())) {
        TerrainFoliageLayer layer = terrain.foliageLayers[layerIndex];
        const std::string prefix = "foliageLayers[" + std::to_string(layerIndex) + "].";
        const auto set = [&](const char* field, float value) {
            CommandHandle::get(sceneProject->id)->addCommand(new PropertyCmd<float>(project, sceneProject->id, entity, ComponentType::TerrainComponent, prefix + field, value));
        };
        const auto setColor = [&](const char* field, const Vector4& value) {
            CommandHandle::get(sceneProject->id)->addCommand(new PropertyCmd<Vector4>(project, sceneProject->id, entity, ComponentType::TerrainComponent, prefix + field, value));
        };
        if (ImGui::BeginTable("##detail_rows", 2, ImGuiTableFlags_SizingStretchProp)) {
            rowLabel("Min/Max Height", "Random height over the width scale.");
            if (ImGui::DragFloatRange2("##detail_ratio", &layer.minHeightRatio, &layer.maxHeightRatio, 0.01f, 0.01f, 1.99f, "%.2f", "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
                set("minHeightRatio", layer.minHeightRatio);
                set("maxHeightRatio", layer.maxHeightRatio);
            }
            rowLabel("Healthy Color");
            if (ImGui::ColorEdit3("##detail_healthy", &layer.healthyColor.x, ImGuiColorEditFlags_Float)) setColor("healthyColor", layer.healthyColor);
            rowLabel("Dry Color");
            if (ImGui::ColorEdit3("##detail_dry", &layer.dryColor.x, ImGuiColorEditFlags_Float)) setColor("dryColor", layer.dryColor);
            rowLabel("Noise Spread", "Noise cells per world unit between Dry and Healthy; 0 picks per instance.");
            if (ImGui::DragFloat("##detail_noise", &layer.noiseSpread, 0.005f, 0.0f, 10.0f, "%.3f")) set("noiseSpread", layer.noiseSpread);
            rowLabel("Bend Factor", "How far the wind moves it; 0 draws the variant without wind.");
            if (ImGui::SliderFloat("##detail_bend", &layer.windBend, 0.0f, 1.0f, "%.2f")) set("windBend", layer.windBend);
            rowLabel("Density Falloff Start", "Share of the draw distance where it starts to thin.");
            if (ImGui::SliderFloat("##detail_falloff", &layer.densityFalloffStart, 0.0f, 1.0f, "%.2f")) set("densityFalloffStart", layer.densityFalloffStart);
            rowLabel("Far Density", "What it thins to at the draw distance.");
            if (ImGui::SliderFloat("##detail_far", &layer.farDensity, 0.0f, 1.0f, "%.2f")) set("farDensity", layer.farDensity);
            ImGui::EndTable();
        }
    }
    if (ImGui::Checkbox("Show Density", &showDensity)) {
        TerrainOverlay::show(sceneProject, entity, showDensity ? TerrainOverlay::Kind::Density : TerrainOverlay::Kind::None, layerIndex, 0.7f);
    }
    drawTerrainSettings(sceneProject, entity, terrain);
    drawStats(sceneProject, entity, terrain);
    drawWarnings(sceneProject, entity, terrain);
}

bool editor::TerrainFoliagePanel::beginStroke(SceneProject* sceneProject, Entity entity, const Vector3& local, float brushSize, bool shift, bool ctrl) {
    strokeActive = true;
    strokeId = nextStrokeId++;
    strokeErase = shift || ctrl;
    strokeEraseSelectedOnly = shift && ctrl;
    random.seed(static_cast<uint32_t>(strokeId) * 2654435761u);
    pendingTrees = 0.0f;
    return stamp(sceneProject, entity, local, brushSize);
}

void editor::TerrainFoliagePanel::commitStamp(SceneProject* sceneProject, Entity entity, TreeDelta&& delta) {
    if (delta.removed.empty() && delta.added.empty()) return;
    const std::string terrainName = sceneProject->scene->getEntityName(entity);
    const std::string name = std::string(strokeErase ? "Erase Trees (" : "Paint Trees (") + terrainName + ")";
    CommandHandle::get(sceneProject->id)->addCommand(new TreeEditCmd(project, sceneProject->id, entity, name, std::move(delta), strokeId));
}

bool editor::TerrainFoliagePanel::stamp(SceneProject* sceneProject, Entity entity, const Vector3& local, float brushSize) {
    if (!strokeActive) return false;
    Scene* scene = sceneProject->scene;
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
    FoliageSystem* foliage = scene->getSystem<FoliageSystem>().get();
    std::shared_ptr<TreeStore> store = foliage->getOrCreateTreeStore(entity);
    if (!store) return false;
    const float size = terrain.terrainSize;

    TreeDelta delta;
    if (strokeErase) {
        const uint32_t onlyId = (strokeEraseSelectedOnly && isReady(terrain)) ? terrain.treePrototypes[prototype].id : UINT32_MAX;
        const float radiusSquared = brushSize * brushSize;
        std::vector<uint32_t> removed;
        for (const TreeRecord& record : store->records()) {
            if (onlyId != UINT32_MAX && record.prototypeId != onlyId) continue;
            const float dx = foliageTreeLocalX(record, size) - local.x;
            const float dz = foliageTreeLocalZ(record, size) - local.z;
            if (dx * dx + dz * dz <= radiusSquared) removed.push_back(record.id);
        }
        if (removed.empty()) return false;
        store->apply(removed, {}, {}, &delta);
        commitStamp(sceneProject, entity, std::move(delta));
        return true;
    }

    if (!isReady(terrain)) return false;
    const TerrainTreePrototype& authored = terrain.treePrototypes[prototype];

    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const float area = 3.14159265f * brushSize * brushSize;
    pendingTrees += std::max(treeDensity * area * 0.05f, 0.05f);
    const int count = static_cast<int>(pendingTrees);
    pendingTrees -= static_cast<float>(count);
    if (count == 0) return false;

    const float half = size * 0.5f;
    const float spacingSquared = spacing * spacing;
    std::vector<TreeRecord> added;
    for (int i = 0; i < count; i++) {
        const float angle = unit(random) * 6.28318530718f;
        const float radius = std::sqrt(unit(random)) * brushSize;
        const float x = local.x + std::cos(angle) * radius;
        const float z = local.z + std::sin(angle) * radius;
        if (x < -half || x > half || z < -half || z > half) continue;

        bool close = false;
        if (spacing > 0.0f) {
            for (const TreeRecord& other : store->records()) {
                const float dx = foliageTreeLocalX(other, size) - x;
                const float dz = foliageTreeLocalZ(other, size) - z;
                if (dx * dx + dz * dz < spacingSquared) {
                    close = true;
                    break;
                }
            }
            for (const TreeRecord& other : added) {
                const float dx = foliageTreeLocalX(other, size) - x;
                const float dz = foliageTreeLocalZ(other, size) - z;
                if (dx * dx + dz * dz < spacingSquared) close = true;
            }
        }
        if (close) continue;

        const SurfaceSample sample = querySurface(terrain, x, z);
        // R7: over a hole there is nothing to stand on
        if (sample.hole) continue;

        TreeRecord record{};
        foliageSetTreeSurface(record, size, terrain.maxHeight, x, z, sample.height);
        const float tall = treeHeight * (1.0f + (unit(random) * 2.0f - 1.0f) * treeHeightRandom);
        const float wide = lockWidthToHeight ? tall : treeWidth * (1.0f + (unit(random) * 2.0f - 1.0f) * treeWidthRandom);
        record.scale = std::max(wide, 0.01f);
        record.heightRatio = static_cast<uint8_t>(std::clamp(tall / record.scale * 128.0f + 0.5f, 1.0f, 255.0f));
        float rotation[4];
        const float up[3] = {0.0f, 1.0f, 0.0f};
        foliageOrientation(randomRotation ? unit(random) : 0.0f, 0.0f, up, rotation);
        foliageQuantizeRotation(rotation, record.rotation);
        record.prototypeId = static_cast<uint16_t>(authored.id);
        record.flags = TREE_SNAP_TO_TERRAIN | treeAlignFlags(normalAlignment);
        const float tint = 1.0f - unit(random) * colorVariation;
        const uint8_t shade = static_cast<uint8_t>(std::clamp(tint * 255.0f + 0.5f, 0.0f, 255.0f));
        record.color[0] = record.color[1] = record.color[2] = shade;
        record.color[3] = 255;
        added.push_back(record);
    }
    if (added.empty()) return false;
    store->apply({}, added, {}, &delta);
    commitStamp(sceneProject, entity, std::move(delta));
    return true;
}

void editor::TerrainFoliagePanel::endStroke() {
    strokeActive = false;
}

bool editor::TerrainFoliagePanel::pickPrototype(SceneProject* sceneProject, Entity entity, const Vector3& local, float radius) {
    Scene* scene = sceneProject->scene;
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
    FoliageSystem* foliage = scene->getSystem<FoliageSystem>().get();
    std::shared_ptr<TreeStore> store = foliage->getTreeStore(entity);
    if (!store) return false;
    float best = radius * radius;
    int found = -1;
    for (const TreeRecord& record : store->records()) {
        const float dx = foliageTreeLocalX(record, terrain.terrainSize) - local.x;
        const float dz = foliageTreeLocalZ(record, terrain.terrainSize) - local.z;
        const float squared = dx * dx + dz * dz;
        if (squared > best) continue;
        for (int i = 0; i < static_cast<int>(terrain.treePrototypes.size()); i++) {
            if (terrain.treePrototypes[i].id == record.prototypeId) {
                best = squared;
                found = i;
                break;
            }
        }
    }
    if (found < 0) return false;
    prototype = found;
    return true;
}

void editor::TerrainFoliagePanel::appendCapsuleLines(const Vector3& base, float radius, float height, std::vector<Vector3>& lines) {
    constexpr int SEGMENTS = 24;
    constexpr float TWO_PI = 6.28318530718f;
    const float bottom = radius;
    const float top = std::max(height - radius, radius);
    for (int i = 0; i < SEGMENTS; i++) {
        const float from = TWO_PI * static_cast<float>(i) / SEGMENTS;
        const float to = TWO_PI * static_cast<float>(i + 1) / SEGMENTS;
        const Vector3 a(std::cos(from) * radius, 0.0f, std::sin(from) * radius);
        const Vector3 b(std::cos(to) * radius, 0.0f, std::sin(to) * radius);
        for (const float ring : {bottom, top}) {
            lines.push_back(base + a + Vector3(0.0f, ring, 0.0f));
            lines.push_back(base + b + Vector3(0.0f, ring, 0.0f));
        }
        // the caps as two arcs each, and the four sides
        if (i < SEGMENTS / 2) {
            const float s0 = std::sin(from) * radius, c0 = std::cos(from) * radius;
            const float s1 = std::sin(to) * radius, c1 = std::cos(to) * radius;
            lines.push_back(base + Vector3(c0, top + s0, 0.0f));
            lines.push_back(base + Vector3(c1, top + s1, 0.0f));
            lines.push_back(base + Vector3(0.0f, top + s0, c0));
            lines.push_back(base + Vector3(0.0f, top + s1, c1));
            lines.push_back(base + Vector3(c0, bottom - s0, 0.0f));
            lines.push_back(base + Vector3(c1, bottom - s1, 0.0f));
            lines.push_back(base + Vector3(0.0f, bottom - s0, c0));
            lines.push_back(base + Vector3(0.0f, bottom - s1, c1));
        }
        if (i % (SEGMENTS / 4) == 0) {
            lines.push_back(base + a + Vector3(0.0f, bottom, 0.0f));
            lines.push_back(base + a + Vector3(0.0f, top, 0.0f));
        }
    }
}

void editor::TerrainFoliagePanel::update() {
    if (massRunner && massRunner->isFinished()) massRunner.reset();
}
