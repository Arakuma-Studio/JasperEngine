// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector's Animation Player layer UI (upstream anim layers spec 9; docs/specs/S21-animation.md
// "Upstream absorption"): the player's Animator Controller row, the Layers table, the clip
// entries' Layer combo, the per-layer States in play mode, the mask coverage lines of the
// Bindings report, and the Users lists of the mask and controller asset views (S21's, in
// InspectorAnimator.cpp). The pure parts (figure, builds, coverage, preview hierarchy) are
// InspectorAnimLayers.{h,cpp}.

#include "InspectorWindow.h"
#include "InspectorAnimLayers.h"

#include "Backend.h"
#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "animation/AnimatorAssetDocument.h"
#include "animation/AnimatorAssetUsers.h"
#include "animation/AnimationClipAppHooks.h"
#include "animation/AnimationClipController.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/ClipBinding.h"
#include "command/CommandHandle.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PlayerLayerActions.h"
#include "command/type/PropertyCmd.h"
#include "component/AnimatorComponent.h"
#include "external/IconsFontAwesome6.h"
#include "object/AnimationPlayer.h"
#include "pool/AnimationClipPool.h"
#include "util/FileDialogs.h"
#include "util/UIProbe.h"
#include "util/Util.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace jasperengine::editor{

    struct InspectorLayerState{
        struct MaskEntry{
            std::shared_ptr<AnimatorAssetDocument> document;
            std::string error;
            double retryAt = 0.0;
        };
        struct CoverageEntry{
            double at = -1.0;
            uint64_t maskRevision = 0;
            animlayersui::MaskCoverage coverage;
        };
        struct UsersEntry{
            double at = -1.0;
            std::vector<AnimatorAssetUser> users;
        };

        fs::path project;
        std::map<std::string, MaskEntry> masks;
        std::map<std::string, CoverageEntry> coverage;
        std::map<std::string, UsersEntry> users;
        std::string error;

        // the layer being renamed (scene, entity, index) and its text
        uint32_t renameScene = 0;
        Entity renameEntity = NULL_ENTITY;
        int renameLayer = -1;
        char renameBuffer[128] = {0};
        bool renameFocus = false;

        void forgetProject(const fs::path& path){
            if (project == path) return;
            masks.clear();
            coverage.clear();
            users.clear();
            project = path;
        }
    };

    void InspectorLayerStateDeleter::operator()(InspectorLayerState* state) const{
        delete state;
    }

}

namespace{

    constexpr double RETRY_SECONDS = 1.0;
    constexpr double COVERAGE_SECONDS = 0.5;
    constexpr size_t MAX_ROWS = 24;

    float iconButtonWidth(const char* icon){
        return ImGui::CalcTextSize(icon).x + ImGui::GetStyle().FramePadding.x * 2;
    }

    // the asset-relative form of a path inside the assets folder; empty when outside
    std::string assetRelative(Project* project, const fs::path& path){
        const fs::path assetsPath = fs::absolute(project->getAssetsPath()).lexically_normal();
        const fs::path filePath = fs::absolute(project->resolveAssetPath(path)).lexically_normal();
        std::error_code ec;
        const fs::path relative = fs::relative(filePath, assetsPath, ec);
        if (ec || relative.empty() || *relative.begin() == "..") return "";
        return relative.generic_string();
    }

    std::string guidOf(Project* project, const std::string& stored){
        return stored.empty() ? std::string() : project->getAssetDatabase().getGuid(project->resolveAssetPath(stored));
    }

    bool fileExists(Project* project, const std::string& stored){
        std::error_code ec;
        return !stored.empty() && fs::is_regular_file(project->resolveAssetPath(stored), ec);
    }

    // a label in a frame like the clip fields: elided text, an optional icon, a color
    void fileLabel(const char* id, const std::string& text, const std::string& tooltip, const ImVec4* color, float width, const char* icon = nullptr){
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::filenameLabel);
        ImGui::BeginChild(id, ImVec2(std::max(width, 1.0f), ImGui::GetFrameHeight()), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y));
        if (color) ImGui::PushStyleColor(ImGuiCol_Text, *color);
        if (icon){
            ImGui::TextUnformatted(icon);
            ImGui::SameLine();
        }
        ImGui::TextUnformatted(text.c_str());
        if (color) ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        if (!tooltip.empty()) ImGui::SetItemTooltip("%s", tooltip.c_str());
    }

    std::vector<std::string> payloadFiles(const ImGuiPayload* payload, bool (*accept)(const std::string&)){
        std::vector<std::string> files;
        if (!payload || !payload->IsDataType("resource_files")) return files;
        for (const std::string& file : Util::getStringsFromPayload(payload)){
            if (accept(file)) files.push_back(file);
        }
        return files;
    }

    bool isMask(const std::string& file){ return Util::isAvatarMaskFile(file); }

    // An asset field (`.mask` / `.controller`): the file name, a picker, a clear button and a drop
    // target. Returns true with `picked` set to the new asset-relative path ("" = cleared).
    bool assetField(Project* project, const char* id, const std::string& stored, bool missing, int dialogFilter,
                    bool (*accept)(const std::string&), bool enabled, std::string& picked, std::string* rejected){
        bool changed = false;
        ImGui::PushID(id);
        const float buttons = iconButtonWidth(ICON_FA_FOLDER_OPEN) + iconButtonWidth(ICON_FA_XMARK) + ImGui::GetStyle().ItemSpacing.x * 2;
        const float width = ImGui::GetContentRegionAvail().x - buttons;
        const ImVec4 warning = Theme::Colors::WarningText;
        const ImVec4 disabled = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
        ImGui::BeginGroup();
        if (stored.empty()){
            fileLabel("field", "< None >", "Drop a file here or pick one", &disabled, width);
        }else if (missing){
            fileLabel("field", fs::path(stored).filename().string(), stored + "\nMissing: the file was deleted or moved outside the editor "
                      "(undoing the delete relinks it)", &warning, width, ICON_FA_TRIANGLE_EXCLAMATION);
        }else{
            fileLabel("field", fs::path(stored).filename().string(), stored, nullptr, width);
        }
        ImGui::EndGroup();
        if (enabled && ImGui::BeginDragDropTarget()){
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
                const std::vector<std::string> files = payloadFiles(payload, accept);
                if (!files.empty()){
                    picked = assetRelative(project, fs::path(files[0]));
                    if (picked.empty()){
                        if (rejected) *rejected = fs::path(files[0]).filename().string() + " is outside the assets folder";
                    }else{
                        changed = true;
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!enabled);
        if (ImGui::Button(ICON_FA_FOLDER_OPEN "##pick")){
            const std::string path = FileDialogs::openFileDialog(project->getAssetsPath().string(), dialogFilter);
            if (!path.empty()){
                picked = assetRelative(project, fs::path(path));
                if (picked.empty() || !accept(picked)){
                    if (rejected) *rejected = fs::path(path).filename().string() + " is not an asset of that type inside the assets folder";
                }else{
                    changed = true;
                }
            }
        }
        ImGui::SetItemTooltip("Choose a file");
        ImGui::SameLine();
        ImGui::BeginDisabled(stored.empty());
        if (ImGui::Button(ICON_FA_XMARK "##clear")){
            picked.clear();
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("None");
        ImGui::EndDisabled();
        ImGui::PopID();
        return changed;
    }

    std::shared_ptr<AnimatorAssetDocument> maskDocument(Project* project, InspectorLayerState& state, const std::string& path, std::string* error){
        if (path.empty()) return nullptr;
        InspectorLayerState::MaskEntry& entry = state.masks[path];
        if (entry.document && entry.document->loadError().empty()) return entry.document;
        const double now = ImGui::GetTime();
        if (!entry.document && now < entry.retryAt){
            if (error) *error = entry.error;
            return nullptr;
        }
        std::string why;
        std::shared_ptr<AnimatorAssetDocument> document = AnimatorAssetDocument::open(project, path, &why);
        if (document && !document->loadError().empty()){
            why = document->loadError();
            document.reset();
        }
        entry.document = document;
        entry.error = why;
        entry.retryAt = now + RETRY_SECONDS;
        if (!document && error) *error = why;
        // (`entry` is gone after this)
        if (state.masks.size() > 64) state.masks.clear();
        return document;
    }

    // the entities a layer's clips bind under the player (its entries' tracks resolved by path)
    std::vector<Entity> layerTargets(Project* project, Scene* scene, Entity root, const AnimationPlayerComponent& player, int layer){
        std::vector<Entity> targets;
        for (const AnimationPlayerClip& entry : player.clips){
            const int entryLayer = entry.layer;
            if (entryLayer != layer || entry.clip.empty()) continue;
            std::string error;
            std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::open(project, entry.clip, &error);
            if (!document) continue;
            for (const ClipTrackData& track : document->data().tracks){
                if (track.channel == ClipChannel::Property) continue;
                const Entity target = track.path.empty() ? root : anim::resolvePath(scene, root, track.path);
                if (target != NULL_ENTITY) targets.push_back(target);
            }
        }
        return targets;
    }

}

InspectorLayerState& editor::InspectorWindow::layerState(){
    if (!layerStatePtr){
        layerStatePtr.reset(new InspectorLayerState());
    }
    layerStatePtr->forgetProject(project->getProjectPath());
    return *layerStatePtr;
}

// ---- the layers' fields ----

// one field of an authored layer (layers[i].<field>); a player with the default layering gets
// its whole list (one Base Layer) so the field has a place
template<class T>
static Command* layerFieldCommand(Project* project, uint32_t sceneId, Entity entity, const AnimationPlayerComponent& player, int layer,
                                  const std::string& field, const T& value, T AnimationPlayerLayer::* member){
    if (player.layers.empty()){
        std::vector<AnimationPlayerLayer> layers = anim::playerLayers(player);
        if (layer < 0 || (size_t)layer >= layers.size()) return nullptr;
        layers[(size_t)layer].*member = value;
        return new PropertyCmd<std::vector<AnimationPlayerLayer>>(project, sceneId, entity, ComponentType::AnimationPlayerComponent, "layers", layers);
    }
    return new PropertyCmd<T>(project, sceneId, entity, ComponentType::AnimationPlayerComponent, "layers[" + std::to_string(layer) + "]." + field, value);
}

void editor::InspectorWindow::setPlayerLayerMask(SceneProject* sceneProject, Entity entity, int layer, const std::string& mask){
    AnimationPlayerComponent* player = sceneProject->scene->findComponent<AnimationPlayerComponent>(entity);
    if (!player) return;
    const uint32_t sceneId = sceneProject->id;
    if (player->layers.empty()){
        std::vector<AnimationPlayerLayer> layers = anim::playerLayers(*player);
        if (layer < 0 || (size_t)layer >= layers.size()) return;
        layers[(size_t)layer].mask = mask;
        layers[(size_t)layer].maskGuid = guidOf(project, mask);
        CommandHandle::get(sceneId)->addCommandNoMerge(
            new PropertyCmd<std::vector<AnimationPlayerLayer>>(project, sceneId, entity, ComponentType::AnimationPlayerComponent, "layers", layers));
        return;
    }
    if (layer < 0 || (size_t)layer >= player->layers.size()) return;
    // the GUID follows the path (spec 6.3: resolved first, the path a hint)
    const std::string prefix = "layers[" + std::to_string(layer) + "]";
    MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
    multiCmd->addPropertyCmd<std::string>(project, sceneId, entity, ComponentType::AnimationPlayerComponent, prefix + ".mask", mask);
    multiCmd->addPropertyCmd<std::string>(project, sceneId, entity, ComponentType::AnimationPlayerComponent, prefix + ".maskGuid", guidOf(project, mask));
    multiCmd->setNoMerge();
    CommandHandle::get(sceneId)->addCommand(multiCmd);
}

// ---- the Animator Controller row ----

void editor::InspectorWindow::drawAnimationPlayerController(ComponentType cpType, SceneProject* sceneProject, Entity entity){
    InspectorLayerState& state = layerState();
    Scene* scene = sceneProject->scene;
    AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(entity);
    if (!player) return;
    const AnimatorComponent* animator = scene->findComponent<AnimatorComponent>(entity);
    const bool stopped = sceneProject->playState == ScenePlayState::STOPPED;
    const uint32_t sceneId = sceneProject->id;
    beginTable(cpType, getLabelSize("Play Automatically"), "player_controller");
    propertyHeader("Controller", -1, false, false);
    UIProbe::item("player.controller");
    if (animator && !player->enabled){
        // upstream's Animation Player with a Controller (the scene migration's): the Animator
        // plays it; play / crossFade / parameters of this player go to the Animator
        ImGui::AlignTextToFramePadding();
        ImGui::TextWrapped(ICON_FA_DIAGRAM_PROJECT " %s plays on the Animator",
                           animator->controller.path.empty() ? "nothing (the Animator has no controller)"
                                                             : fs::path(animator->controller.path).filename().string().c_str());
        ImGui::SetItemTooltip("This player is disabled: its calls (play, crossFade, setFloat...) go to the GameObject's Animator. "
                              "Enable it to play its own clips again.");
    }else{
        ImGui::BeginDisabled(!stopped || player->clips.empty());
        if (ImGui::Button("Create Animator Controller from Clips##player_controller_create", ImVec2(ImGui::GetContentRegionAvail().x, 0))){
            const std::string directory = project->normalizeToProjectRelative(project->getAssetsPath()).generic_string();
            const std::string name = scene->getEntityName(entity);
            const std::string suggested = animlayersui::freeAssetPath(project, directory, name.empty() ? "New Animator Controller" : name, anim::CONTROLLER_EXTENSION);
            const std::string chosen = FileDialogs::saveFileDialog(project->getAssetsPath().string(), fs::u8path(suggested).filename().string(),
                                                                   FILE_DIALOG_ANIMATOR_CONTROLLER);
            if (!chosen.empty()){
                std::string path = project->normalizeToProjectRelative(fs::u8path(chosen)).generic_string();
                if (!Util::isAnimatorControllerFile(path)) path += anim::CONTROLLER_EXTENSION;
                std::string error;
                if (!animlayersui::createControllerFromClips(project, sceneId, entity, path, error)) state.error = error;
            }
        }
        ImGui::EndDisabled();
        UIProbe::item("player.controller.create");
        ImGui::SetItemTooltip("Make an Animator Controller with one state per clip entry (the entries' layers as its layers), play it on an "
                              "Animator of this GameObject and let this player hand over to it (one undo step)");
    }
    endTable();
}

// ---- the Layers table ----

void editor::InspectorWindow::drawAnimationPlayerLayers(ComponentType cpType, SceneProject* sceneProject, Entity entity){
    InspectorLayerState& state = layerState();
    Scene* scene = sceneProject->scene;
    AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(entity);
    if (!player) return;
    // copies: the commands below change the component while the rows are drawn
    const AnimationPlayerComponent snapshot = *player;
    const std::vector<AnimationPlayerLayer> layers = anim::playerLayers(snapshot);
    const bool stopped = sceneProject->playState == ScenePlayState::STOPPED;
    const uint32_t sceneId = sceneProject->id;
    const float labelWidth = getLabelSize("Play Automatically");
    std::string error;

    // "Layers (1)" collapsed while only the base layer exists
    ImGui::SetNextItemOpen(layers.size() > 1, ImGuiCond_Once);
    const std::string header = "Layers (" + std::to_string(layers.size()) + ")##player_layers";
    const bool open = ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding);
    UIProbe::item("player.layers");
    ImGui::SetItemTooltip("Animation layers: each clip entry plays on one; a layer has a weight, a blending (Override / Additive) and an Avatar Mask");
    if (!open) return;

    int moveFrom = -1, moveTo = -1;
    beginTable(cpType, labelWidth, "player_layers_table");
    for (size_t i = 0; i < layers.size(); i++){
        const AnimationPlayerLayer& layer = layers[i];
        const int index = (int)i;
        const bool base = i == 0;
        ImGui::PushID(index);
        const std::string prefix = "layers[" + std::to_string(i) + "]";

        // the row: drag handle + name (right-click for the menu); a rename edits it in place
        prefabRowProperty = snapshot.layers.empty() ? std::string("layers") : prefix;
        propertyHeader(base ? layer.name + " (Base)" : layer.name, -1, false, false);
        const bool renaming = state.renameScene == sceneId && state.renameEntity == entity && state.renameLayer == index;
        if (!base){
            ImGui::BeginDisabled(!stopped);
            ImGui::Button(ICON_FA_GRIP_LINES "##layer_handle");
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Drag to reorder (the layer's clip entries follow it)");
            if (stopped && ImGui::BeginDragDropSource()){
                ImGui::SetDragDropPayload("player_layer", &index, sizeof(int));
                ImGui::Text("%s", layer.name.c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::SameLine();
        }
        if (stopped && !base && ImGui::BeginDragDropTarget()){
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("player_layer")){
                moveFrom = *(const int*)payload->Data;
                moveTo = index;
            }
            ImGui::EndDragDropTarget();
        }
        if (renaming){
            if (state.renameFocus){
                ImGui::SetKeyboardFocusHere();
                state.renameFocus = false;
            }
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            const bool enter = ImGui::InputText("##layer_rename", state.renameBuffer, sizeof(state.renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
            if (enter || ImGui::IsItemDeactivated()){
                const std::string name = state.renameBuffer;
                state.renameLayer = -1;
                if (!ImGui::IsKeyPressed(ImGuiKey_Escape) && name != layer.name &&
                    !PlayerLayerActions::renameLayer(project, sceneId, entity, index, name, &error)){
                    state.error = error;
                }
            }
        }else{
            ImGui::AlignTextToFramePadding();
            if (base){
                ImGui::TextDisabled("weight 1, override");
            }else{
                ImGui::TextDisabled("%s, %s%s", anim::layerBlendName(layer.blend), layer.mask.empty() ? "no mask" : "mask ",
                                    layer.mask.empty() ? "" : fs::path(layer.mask).filename().string().c_str());
            }
        }
        if (ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()) && ImGui::IsMouseReleased(ImGuiMouseButton_Right)){
            ImGui::OpenPopup("layer_menu");
        }
        if (ImGui::BeginPopup("layer_menu")){
            ImGui::TextDisabled("%s", layer.name.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Rename", nullptr, false, stopped && !base)){
                state.renameScene = sceneId;
                state.renameEntity = entity;
                state.renameLayer = index;
                state.renameFocus = true;
                std::snprintf(state.renameBuffer, sizeof(state.renameBuffer), "%s", layer.name.c_str());
            }
            if (ImGui::MenuItem("Move Up", nullptr, false, stopped && i > 1)){
                moveFrom = index;
                moveTo = index - 1;
            }
            if (ImGui::MenuItem("Move Down", nullptr, false, stopped && !base && i + 1 < layers.size())){
                moveFrom = index;
                moveTo = index + 1;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Create Mask from Selection...", nullptr, false, stopped)){
                const std::string directory = project->normalizeToProjectRelative(project->getAssetsPath()).generic_string();
                const std::string suggested = animlayersui::freeAssetPath(project, directory, layer.name, anim::MASK_EXTENSION);
                const std::string chosen = FileDialogs::saveFileDialog(project->getAssetsPath().string(), fs::u8path(suggested).filename().string(),
                                                                       FILE_DIALOG_AVATAR_MASK);
                if (!chosen.empty()){
                    std::string path = project->normalizeToProjectRelative(fs::u8path(chosen)).generic_string();
                    if (!Util::isAvatarMaskFile(path)) path += anim::MASK_EXTENSION;
                    if (!animlayersui::createMaskFromSelection(project, sceneId, entity, index, path, project->getSelectedEntities(sceneId), error)){
                        state.error = error;
                    }
                }
            }
            ImGui::SetItemTooltip("Write a mask that animates the selected GameObjects (and what is below them) and use it on this layer (one undo step)");
            ImGui::Separator();
            if (ImGui::MenuItem("Remove", nullptr, false, stopped && !base)){
                if (!PlayerLayerActions::removeLayer(project, sceneId, entity, index, &error)) state.error = error;
            }
            ImGui::EndPopup();
        }

        // Weight
        prefabRowProperty = snapshot.layers.empty() ? std::string("layers") : prefix + ".weight";
        propertyHeader("Weight", -1, !base && layer.weight != 1.0f, false, true);
        if (base){
            ImGui::BeginDisabled();
            float one = 1.0f;
            ImGui::SliderFloat("##layer_weight", &one, 0.0f, 1.0f, "1 (Base)");
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("The base layer's weight is always 1");
        }else{
            float weight = layer.weight;
            const bool weightChanged = ImGui::SliderFloat("##layer_weight", &weight, 0.0f, 1.0f, "%.2f");
            UIProbe::item("player.layer." + std::to_string(index) + ".weight");
            if (weightChanged){
                weight = std::clamp(weight, 0.0f, 1.0f);
                cmd = layerFieldCommand(project, sceneId, entity, snapshot, index, "weight", weight, &AnimationPlayerLayer::weight);
                if (cmd) CommandHandle::get(sceneId)->addCommand(cmd);
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && cmd){
                cmd->setNoMerge();
                cmd = nullptr;
            }
            ImGui::SetItemTooltip("How much of the layer shows over the layers below (setLayerWeight changes it at runtime)");
        }

        // Blending
        prefabRowProperty = snapshot.layers.empty() ? std::string("layers") : prefix + ".blend";
        propertyHeader("Blending", -1, false, false, true);
        ImGui::BeginDisabled(base || !stopped);
        const char* blendNames[] = {"Override", "Additive"};
        int blend = base ? 0 : (int)layer.blend;
        if (ImGui::Combo("##layer_blend", &blend, blendNames, 2) && !base){
            Command* blendCmd = layerFieldCommand(project, sceneId, entity, snapshot, index, "blend", (AnimationLayerBlend)blend, &AnimationPlayerLayer::blend);
            if (blendCmd) CommandHandle::get(sceneId)->addCommandNoMerge(blendCmd);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(base ? "The base layer always overrides" :
                              "Override replaces what the layers below produce; Additive adds the clip's change from its first frame");

        // Mask
        prefabRowProperty = snapshot.layers.empty() ? std::string("layers") : prefix + ".mask";
        propertyHeader("Mask", -1, false, false, true);
        std::string picked;
        std::string rejected;
        if (assetField(project, "layer_mask", layer.mask, !layer.mask.empty() && !fileExists(project, layer.mask), FILE_DIALOG_AVATAR_MASK,
                       isMask, stopped, picked, &rejected)){
            setPlayerLayerMask(sceneProject, entity, index, picked);
        }
        if (!rejected.empty()) state.error = rejected;
        ImGui::PopID();
    }
    endTable();

    beginTable(cpType, labelWidth, "player_layers_add");
    propertyHeader("");
    ImGui::BeginDisabled(!stopped);
    if (ImGui::Button(ICON_FA_PLUS " Add Layer##player_layer_add", ImVec2(ImGui::GetContentRegionAvail().x, 0))){
        AnimationPlayerLayer added;
        std::set<std::string> names;
        for (const AnimationPlayerLayer& layer : layers) names.insert(layer.name);
        int k = 1;
        do{
            added.name = "New Layer" + (k == 1 ? std::string() : " " + std::to_string(k));
            k++;
        }while (names.count(added.name));
        if (!PlayerLayerActions::addLayer(project, sceneId, entity, added, &error)) state.error = error;
    }
    ImGui::EndDisabled();
    UIProbe::item("player.addLayer");
    endTable();
    if (moveFrom > 0 && moveTo > 0 && moveFrom != moveTo){
        if (!PlayerLayerActions::moveLayer(project, sceneId, entity, moveFrom, moveTo, &error)) state.error = error;
    }

    if (!state.error.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("%s", state.error.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton(ICON_FA_XMARK " Dismiss##player_layer_error")) state.error.clear();
    }
    ImGui::TreePop();
}

// the clip entry's Layer combo (a row of the entry)
void editor::InspectorWindow::drawPlayerClipLayerRow(ComponentType cpType, SceneProject* sceneProject, Entity entity, size_t index){
    AnimationPlayerComponent* player = sceneProject->scene->findComponent<AnimationPlayerComponent>(entity);
    if (!player || index >= player->clips.size()) return;
    const std::vector<AnimationPlayerLayer> layers = anim::playerLayers(*player);
    const int layer = player->clips[index].layer;
    const std::string prefix = "clips[" + std::to_string(index) + "]";
    prefabRowProperty = prefix + ".layer";
    if (propertyHeader("Layer", -1, layer != 0, false, true)){
        CommandHandle::get(sceneProject->id)->addCommandNoMerge(new PropertyCmd<int>(project, sceneProject->id, entity, cpType, prefix + ".layer", 0));
    }
    const bool known = layer >= 0 && (size_t)layer < layers.size();
    const std::string current = known ? layers[(size_t)layer].name : "Layer " + std::to_string(layer) + " (missing: plays on the base)";
    ImGui::BeginDisabled(sceneProject->playState != ScenePlayState::STOPPED);
    const bool comboOpen = ImGui::BeginCombo("##clip_layer", current.c_str());
    UIProbe::item("player.clip." + std::to_string(index) + ".layer");
    if (comboOpen){
        for (size_t l = 0; l < layers.size(); l++){
            const bool picked = ImGui::Selectable((layers[l].name + "##layer" + std::to_string(l)).c_str(), (int)l == layer);
            UIProbe::item("player.clip." + std::to_string(index) + ".layerOption." + std::to_string(l));
            if (picked && (int)l != layer){
                CommandHandle::get(sceneProject->id)->addCommandNoMerge(
                    new PropertyCmd<int>(project, sceneProject->id, entity, cpType, prefix + ".layer", (int)l));
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("The layer this state plays on");
}

// ---- Bindings: the masks' coverage ----

void editor::InspectorWindow::drawPlayerMaskCoverage(SceneProject* sceneProject, Entity entity){
    InspectorLayerState& state = layerState();
    Scene* scene = sceneProject->scene;
    const AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(entity);
    if (!player) return;
    const std::vector<AnimationPlayerLayer> layers = anim::playerLayers(*player);
    for (size_t l = 0; l < layers.size(); l++){
        const AnimationPlayerLayer& layer = layers[l];
        if (layer.mask.empty()) continue;
        ImGui::PushID((int)l + 1000);
        std::string error;
        std::shared_ptr<AnimatorAssetDocument> document = maskDocument(project, state, layer.mask, &error);
        if (!document){
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s: mask: %s (the layer contributes nothing)", layer.name.c_str(),
                               fileExists(project, layer.mask) ? error.c_str() : "missing");
            ImGui::PopID();
            continue;
        }
        const std::string key = std::to_string(sceneProject->id) + ":" + std::to_string(entity) + ":" + std::to_string(l) + ":" + layer.mask;
        InspectorLayerState::CoverageEntry& entry = state.coverage[key];
        const double now = ImGui::GetTime();
        if (entry.at < 0.0 || entry.maskRevision != document->revision() || now - entry.at > COVERAGE_SECONDS){
            if (state.coverage.size() > 64){
                state.coverage.clear();
                ImGui::PopID();
                break;
            }
            entry.coverage = animlayersui::maskCoverage(scene, entity, document->mask(), layerTargets(project, scene, entity, *player, (int)l));
            entry.maskRevision = document->revision();
            entry.at = now;
        }
        const animlayersui::MaskCoverage& coverage = entry.coverage;
        ImGui::Text("%s: mask: %zu/%zu bound entities covered", layer.name.c_str(), coverage.covered, coverage.total);
        ImGui::SetItemTooltip("%s", layer.mask.c_str());
        ImGui::Indent();
        if (coverage.humanoidWithoutRig){
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
            ImGui::TextWrapped("its body parts need a humanoid rig on this GameObject (%s): its transforms and default apply", coverage.rigError.c_str());
            ImGui::PopStyleColor();
        }
        size_t shown = 0;
        for (const ClipWarning& warning : document->warnings()){
            if (shown++ >= MAX_ROWS) break;
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
            ImGui::TextWrapped("%s%s%s", warning.path.c_str(), warning.path.empty() ? "" : ": ", warning.message.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Unindent();
        ImGui::PopID();
    }
}

// ---- play mode: the States panel grouped by layer ----

void editor::InspectorWindow::drawPlayerLayerStatesHeader(SceneProject* sceneProject, Entity entity, int layer){
    Scene* scene = sceneProject->scene;
    AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(entity);
    if (!player || !player->runtime) return;
    anim::AnimationPlayerRuntime& runtime = *player->runtime;
    if (layer < 0 || (size_t)layer >= runtime.layers.size()) return;
    const anim::LayerRuntime& layerRuntime = runtime.layers[(size_t)layer];
    ImGui::PushID(layer + 2000);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(layerRuntime.name.c_str());
    ImGui::SameLine();
    if (layer == 0){
        ImGui::ProgressBar(1.0f, ImVec2(ImGui::GetContentRegionAvail().x, 0), "1 (Base)");
    }else{
        // the runtime weight (setLayerWeight): not saved, no undo
        float weight = layerRuntime.weight;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::SliderFloat("##live_layer_weight", &weight, 0.0f, 1.0f, "w %.2f")){
            anim::setLayerWeight(*player, layer, weight, 0.0f, scene->getEntityName(entity));
        }
        ImGui::SetItemTooltip("The layer's runtime weight (like setLayerWeight from Lua; not saved)%s%s", layerRuntime.maskPath.empty() ? "" : "\nmask ",
                              layerRuntime.maskPath.c_str());
    }
    ImGui::PopID();
}

// ---- Users (the mask and controller asset views, InspectorAnimator.cpp) ----

void editor::InspectorWindow::drawAnimatorAssetUsers(const std::string& projectPath, bool mask){
    InspectorLayerState& state = layerState();
    InspectorLayerState::UsersEntry& entry = state.users[(mask ? "mask:" : "controller:") + projectPath];
    if (entry.at < 0.0 || ImGui::GetTime() - entry.at > RETRY_SECONDS){
        entry.users = mask ? findMaskUsers(project, projectPath) : findControllerUsers(project, projectPath);
        entry.at = ImGui::GetTime();
    }
    ImGui::SeparatorText("Users");
    UIProbe::item(mask ? "asset.mask.users" : "asset.controller.users");
    if (entry.users.empty()){
        ImGui::TextDisabled(mask ? "No Animation Player layer or controller layer uses this mask" : "No Animator uses this controller");
        return;
    }
    size_t shown = 0;
    for (const AnimatorAssetUser& user : entry.users){
        if (shown++ >= MAX_ROWS){
            ImGui::TextDisabled("... and %zu more", entry.users.size() - MAX_ROWS);
            break;
        }
        std::string text;
        if (!user.controllerFile.empty()){
            text = ICON_FA_DIAGRAM_PROJECT " " + user.controllerFile;
        }else if (user.prefabAsset){
            text = ICON_FA_CUBE " " + user.prefabPath;
        }else if (SceneProject* scene = project->getScene(user.sceneId)){
            const std::string name = scene->scene && scene->scene->isEntityCreated(user.entity) ? scene->scene->getEntityName(user.entity) : std::string("?");
            text = ICON_FA_PERSON " " + name + " (" + scene->name + ")";
        }
        if (user.layer >= 0) text += "  layer " + std::to_string(user.layer);
        ImGui::BulletText("%s", text.c_str());
    }
}
