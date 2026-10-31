// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector's animation layer UI (anim layers spec 9, 11.4): the Animation Player's
// Controller field and Layers table, the clip entries' Layer combo, the per-layer States in play
// mode, the mask coverage lines of the Bindings report, and the Avatar Mask / Animator Controller
// asset views. The pure parts (figure, builds, coverage, preview hierarchy) are
// InspectorAnimLayers.{h,cpp}.

#include "InspectorWindow.h"
#include "InspectorAnimLayers.h"

#include "Backend.h"
#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "animation/AnimAssetDocument.h"
#include "animation/AnimationClipAppHooks.h"
#include "animation/AnimationClipController.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/ClipBinding.h"
#include "command/CommandHandle.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PlayerLayerActions.h"
#include "command/type/PropertyCmd.h"
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
            std::shared_ptr<AvatarMaskDocument> document;
            std::string error;
            double retryAt = 0.0;
        };
        struct ControllerEntry{
            std::shared_ptr<AnimatorControllerDocument> document;
            std::string error;
            double retryAt = 0.0;
            uint64_t validatedRevision = 0;
            double validatedAt = -1.0;
            std::vector<ClipWarning> warnings;
        };
        struct CoverageEntry{
            double at = -1.0;
            uint64_t maskRevision = 0;
            animlayersui::MaskCoverage coverage;
        };
        struct UsersEntry{
            double at = -1.0;
            std::vector<AnimAssetUser> users;
        };

        fs::path project;
        std::map<std::string, MaskEntry> masks;
        std::map<std::string, ControllerEntry> controllers;
        std::map<std::string, CoverageEntry> coverage;
        std::map<std::string, UsersEntry> users;
        std::string error;

        // the layer being renamed (scene, entity, index) and its text
        uint32_t renameScene = 0;
        Entity renameEntity = NULL_ENTITY;
        int renameLayer = -1;
        char renameBuffer[128] = {0};
        bool renameFocus = false;

        // the mask asset view: its preview hierarchy (by source key), the part last clicked
        std::string previewKey;
        double previewAt = -1.0;
        animlayersui::PreviewHierarchy preview;
        int selectedPart = -1;
        bool useSelection = false;
        std::set<std::string> collapsed;
        uint64_t maskBuiltRevision = 0;
        std::string maskBuiltPath;
        std::shared_ptr<const anim::AvatarMask> maskBuilt;

        void forgetProject(const fs::path& path){
            if (project == path) return;
            masks.clear();
            controllers.clear();
            coverage.clear();
            users.clear();
            preview = animlayersui::PreviewHierarchy();
            previewKey.clear();
            maskBuilt.reset();
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

    const ImVec4 OK_COLOR(0.45f, 0.8f, 0.45f, 1.0f);

    float textWidth(const std::string& text){
        return ImGui::CalcTextSize(text.c_str()).x;
    }

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
    bool isController(const std::string& file){ return Util::isAnimatorControllerFile(file); }
    bool isModelOrPrefab(const std::string& file){ return Util::isModelFile(file) || Util::isPrefabFile(file); }

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

    std::shared_ptr<AvatarMaskDocument> maskDocument(Project* project, InspectorLayerState& state, const std::string& path, std::string* error){
        if (path.empty()) return nullptr;
        InspectorLayerState::MaskEntry& entry = state.masks[path];
        if (entry.document && entry.document->exists()) return entry.document;
        const double now = ImGui::GetTime();
        if (!entry.document && now < entry.retryAt){
            if (error) *error = entry.error;
            return nullptr;
        }
        std::string why;
        std::shared_ptr<AvatarMaskDocument> document = AvatarMaskDocument::open(project, path, &why);
        entry.document = document;
        entry.error = why;
        entry.retryAt = now + RETRY_SECONDS;
        if (!document && error) *error = why;
        // (`entry` is gone after this)
        if (state.masks.size() > 64) state.masks.clear();
        return document;
    }

    std::shared_ptr<AnimatorControllerDocument> controllerDocument(Project* project, InspectorLayerState& state, const std::string& path,
                                                                   std::string* error){
        if (path.empty()) return nullptr;
        InspectorLayerState::ControllerEntry& entry = state.controllers[path];
        if (entry.document && entry.document->exists()) return entry.document;
        const double now = ImGui::GetTime();
        if (!entry.document && now < entry.retryAt){
            if (error) *error = entry.error;
            return nullptr;
        }
        std::string why;
        entry.document = AnimatorControllerDocument::open(project, path, &why);
        entry.error = why;
        entry.retryAt = now + RETRY_SECONDS;
        if (!entry.document && error) *error = why;
        return entry.document;
    }

    // the controller's validation (spec 5.11), clips resolved through their documents; cached per revision
    const std::vector<ClipWarning>& controllerWarnings(Project* project, InspectorLayerState& state, const std::string& path,
                                                       AnimatorControllerDocument& document){
        InspectorLayerState::ControllerEntry& entry = state.controllers[path];
        const double now = ImGui::GetTime();
        if (entry.validatedRevision == document.revision() && now - entry.validatedAt < RETRY_SECONDS) return entry.warnings;
        entry.validatedRevision = document.revision();
        entry.validatedAt = now;
        entry.warnings = anim::validateController(document.data(), [project](const std::string& clip){
            anim::ControllerClipInfo info;
            std::string error;
            std::shared_ptr<AnimationClipDocument> clipDocument = AnimationClipDocument::open(project, clip, &error);
            info.loads = clipDocument != nullptr;
            info.error = error;
            if (clipDocument){
                const ClipWrapMode wrap = clipDocument->data().wrap;
                info.loops = wrap == ClipWrapMode::Loop || wrap == ClipWrapMode::PingPong;
            }
            return info;
        });
        for (const ClipWarning& warning : document.loadWarnings()) entry.warnings.insert(entry.warnings.begin(), warning);
        return entry.warnings;
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

    ImU32 weightColor(float weight, float alpha = 1.0f){
        weight = std::clamp(weight, 0.0f, 1.0f);
        const ImVec4 off(0.78f, 0.27f, 0.25f, alpha);
        const ImVec4 on(0.30f, 0.72f, 0.36f, alpha);
        return ImGui::ColorConvertFloat4ToU32(ImVec4(off.x + (on.x - off.x) * weight, off.y + (on.y - off.y) * weight, off.z + (on.z - off.z) * weight, alpha));
    }

}

InspectorLayerState& editor::InspectorWindow::layerState(){
    if (!layerStatePtr){
        layerStatePtr.reset(new InspectorLayerState());
    }
    layerStatePtr->forgetProject(project->getProjectPath());
    return *layerStatePtr;
}

// ---- the Animation Player's Controller field ----

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

void editor::InspectorWindow::drawAnimationPlayerController(ComponentType cpType, SceneProject* sceneProject, Entity entity){
    InspectorLayerState& state = layerState();
    AnimationPlayerComponent* player = sceneProject->scene->findComponent<AnimationPlayerComponent>(entity);
    if (!player) return;
    const bool stopped = sceneProject->playState == ScenePlayState::STOPPED;
    const std::string controller = player->controller;
    const AnimationClipAppHooks& hooks = animationClipAppHooks();

    beginTable(cpType, getLabelSize("Play Automatically"), "player_controller");
    prefabRowProperty = "controller";
    propertyHeader("Controller", -1, false, false);
    std::string picked;
    std::string rejected;
    if (assetField(project, "player_controller_field", controller, !controller.empty() && !fileExists(project, controller),
                   FILE_DIALOG_ANIMATOR_CONTROLLER, isController, stopped, picked, &rejected)){
        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
        multiCmd->addPropertyCmd<std::string>(project, sceneProject->id, entity, cpType, "controller", picked);
        multiCmd->addPropertyCmd<std::string>(project, sceneProject->id, entity, cpType, "controllerGuid", guidOf(project, picked));
        multiCmd->setNoMerge();
        CommandHandle::get(sceneProject->id)->addCommand(multiCmd);
    }
    UIProbe::item("player.controller");
    if (!rejected.empty()) state.error = rejected;
    ImGui::SetItemTooltip("An Animator Controller (.controller) plays the states instead of the clip list");

    propertyHeader("");
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImGui::BeginDisabled(controller.empty() || !hooks.openAnimator);
    if (ImGui::Button("Open##player_controller_open", ImVec2(half, 0)) && hooks.openAnimator){
        hooks.openAnimator(project, project->normalizeToProjectRelative(project->resolveAssetPath(controller)).generic_string(),
                           sceneProject->id, entity);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Edit the controller in the Animator window (Window > Animation > Animator)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!stopped || player->clips.empty());
    if (ImGui::Button("Create Controller from Clips##player_controller_create", ImVec2(half, 0))){
        // next to the first .anim entry, else the assets folder
        std::string directory = project->normalizeToProjectRelative(project->getAssetsPath()).generic_string();
        for (const AnimationPlayerClip& entry : player->clips){
            if (Util::isAnimationClipFile(entry.clip)){
                directory = project->normalizeToProjectRelative(project->resolveAssetPath(entry.clip).parent_path()).generic_string();
                break;
            }
        }
        const std::string name = sceneProject->scene->getEntityName(entity);
        const std::string suggested = animlayersui::freeAssetPath(project, directory, name.empty() ? "New Animator Controller" : name, anim::CONTROLLER_EXTENSION);
        const std::string chosen = FileDialogs::saveFileDialog((project->getProjectPath() / fs::u8path(directory)).string(),
                                                               fs::u8path(suggested).filename().string(), FILE_DIALOG_ANIMATOR_CONTROLLER);
        if (!chosen.empty()){
            std::string path = project->normalizeToProjectRelative(fs::u8path(chosen)).generic_string();
            if (!Util::isAnimatorControllerFile(path)) path += anim::CONTROLLER_EXTENSION;
            std::string error;
            if (!animlayersui::createControllerFromClips(project, sceneProject->id, entity, path, error)) state.error = error;
        }
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Make a controller with one state per clip entry (the entries' layers as its layers) and use it (one undo step)");
    endTable();

    if (controller.empty()) return;
    // the controller owns the states (spec 5.10)
    const std::vector<AnimationPlayerLayer> layers = player->layers;
    if (!player->clips.empty() || layers.size() > 1){
        const size_t clipCount = player->clips.size();
        const size_t layerCount = layers.size();
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %zu clip entr%s and %zu layer%s are ignored: the controller owns the states and layers.",
                           clipCount, clipCount == 1 ? "y" : "ies", layerCount, layerCount == 1 ? "" : "s");
        ImGui::PopStyleColor();
    }
    std::string error;
    std::shared_ptr<AnimatorControllerDocument> document = controllerDocument(project, state, controller, &error);
    if (!document){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("%s", fileExists(project, controller) ? error.c_str() : "The controller file is missing.");
        ImGui::PopStyleColor();
        return;
    }
    const std::vector<ClipWarning>& warnings = controllerWarnings(project, state, controller, *document);
    const anim::ControllerData& data = document->data();
    size_t states = 0;
    for (const anim::ControllerLayer& layer : data.layers) states += layer.states.size();
    ImGui::TextDisabled("%zu parameter%s, %zu layer%s, %zu state%s", data.parameters.size(), data.parameters.size() == 1 ? "" : "s",
                        data.layers.size(), data.layers.size() == 1 ? "" : "s", states, states == 1 ? "" : "s");
    if (!warnings.empty()){
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
        const std::string label = std::string("Controller  " ICON_FA_TRIANGLE_EXCLAMATION " ") + std::to_string(warnings.size()) +
                                  (warnings.size() == 1 ? " problem" : " problems") + "##player_controller_warnings";
        const bool open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding);
        ImGui::PopStyleColor();
        if (open){
            size_t shown = 0;
            for (const ClipWarning& warning : warnings){
                if (shown++ >= MAX_ROWS){
                    ImGui::TextDisabled("... and %zu more", warnings.size() - MAX_ROWS);
                    break;
                }
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
                ImGui::TextWrapped("%s%s%s", warning.path.c_str(), warning.path.empty() ? "" : ": ", warning.message.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::TreePop();
        }
    }
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
            if (ImGui::SliderFloat("##layer_weight", &weight, 0.0f, 1.0f, "%.2f")){
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
    if (ImGui::BeginCombo("##clip_layer", current.c_str())){
        for (size_t l = 0; l < layers.size(); l++){
            if (ImGui::Selectable((layers[l].name + "##layer" + std::to_string(l)).c_str(), (int)l == layer) && (int)l != layer){
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
    if (!player || !player->controller.empty()) return;
    const std::vector<AnimationPlayerLayer> layers = anim::playerLayers(*player);
    for (size_t l = 0; l < layers.size(); l++){
        const AnimationPlayerLayer& layer = layers[l];
        if (layer.mask.empty()) continue;
        ImGui::PushID((int)l + 1000);
        std::string error;
        std::shared_ptr<AvatarMaskDocument> document = maskDocument(project, state, layer.mask, &error);
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
            std::shared_ptr<const anim::AvatarMask> mask = anim::AvatarMask::build(document->data());
            entry.coverage = animlayersui::maskCoverage(scene, entity, *mask, layerTargets(project, scene, entity, *player, (int)l));
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
        for (const ClipWarning& warning : document->loadWarnings()){
            if (shown++ >= MAX_ROWS) break;
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
            ImGui::TextWrapped("%s%s%s", warning.path.c_str(), warning.path.empty() ? "" : ": ", warning.message.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Unindent();
        ImGui::PopID();
    }
}

// ---- play mode: the controller's layers and parameters ----

void editor::InspectorWindow::drawControllerPlayState(SceneProject* sceneProject, Entity entity){
    Scene* scene = sceneProject->scene;
    AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(entity);
    if (!player || !player->runtime || !player->runtime->controller || !player->runtime->controller->controller) return;
    const anim::ControllerRuntime& controller = *player->runtime->controller;
    const anim::ControllerData& data = controller.controller->getData();
    ImGui::SeparatorText("Controller");
    UIProbe::item("player.controllerState");
    for (size_t l = 0; l < data.layers.size() && l < controller.layers.size(); l++){
        const anim::ControllerStateView current = anim::controllerStateInfo(*player, (int)l, false);
        const anim::ControllerStateView next = anim::controllerStateInfo(*player, (int)l, true);
        ImGui::Text("%s (w %.2f):", data.layers[l].name.c_str(), player->runtime->layerWeight((int)l));
        ImGui::SameLine();
        if (!current.valid){
            ImGui::TextDisabled("stopped");
            continue;
        }
        if (next.valid){
            const anim::ControllerTransitionRuntime& transition = controller.layers[l].transition;
            const float progress = transition.duration > 0.0f ? std::clamp(transition.elapsed / transition.duration, 0.0f, 1.0f) : 1.0f;
            ImGui::Text("%s " ICON_FA_ARROW_RIGHT " %s (%.0f%%)", current.name.c_str(), next.name.c_str(), progress * 100.0f);
        }else{
            ImGui::Text("%s  %.2f", current.name.c_str(), current.normalizedTime);
        }
    }
    if (!data.parameters.empty()){
        ImGui::TextDisabled("Parameters (edit them in the Animator window)");
        ImGui::Indent();
        for (size_t p = 0; p < data.parameters.size() && p < controller.params.size(); p++){
            const anim::ControllerParameter& parameter = data.parameters[p];
            const float value = controller.params[p].value;
            switch (parameter.type){
                case anim::ControllerParameterType::Float: ImGui::Text("%s = %.3f", parameter.name.c_str(), value); break;
                case anim::ControllerParameterType::Int: ImGui::Text("%s = %d", parameter.name.c_str(), (int)value); break;
                case anim::ControllerParameterType::Bool: ImGui::Text("%s = %s", parameter.name.c_str(), value != 0.0f ? "true" : "false"); break;
                case anim::ControllerParameterType::Trigger: ImGui::Text("%s %s", parameter.name.c_str(), value != 0.0f ? "(set)" : "(trigger)"); break;
            }
        }
        ImGui::Unindent();
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

// ---- the Avatar Mask asset view ----

void editor::InspectorWindow::drawAvatarMaskAsset(const fs::path& asset){
    InspectorLayerState& state = layerState();
    drawAssetHeader(ICON_FA_CHILD_REACHING, asset, "Avatar Mask");
    const std::string path = project->normalizeToProjectRelative(asset).generic_string();
    std::string error;
    std::shared_ptr<AvatarMaskDocument> document = maskDocument(project, state, path, &error);
    if (!document){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("%s", error.c_str());
        ImGui::PopStyleColor();
        return;
    }
    const anim::AvatarMaskData data = document->data();
    auto commit = [&](const anim::AvatarMaskData& edited, const std::string& mergeKey){
        std::string failure;
        const uint64_t clock = AnimationClipController::clipCommitClock(project);
        if (!document->commit(edited, "Edit Avatar Mask", mergeKey, &failure)){
            state.error = failure;
            return;
        }
        // an animation asset write of the editor UI: Ctrl+Z undoes it from any window
        AnimationClipController::markClipCommit(clock);
    };

    // Default
    ImGui::SeparatorText("Default");
    float defaultWeight = data.defaultWeight;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::SliderFloat("##mask_default", &defaultWeight, 0.0f, 1.0f, "Default %.2f")){
        anim::AvatarMaskData edited = data;
        edited.defaultWeight = std::clamp(defaultWeight, 0.0f, 1.0f);
        commit(edited, "mask-default");
    }
    UIProbe::item("mask.default");
    ImGui::SetItemTooltip("The weight of every bone no rule below covers (1: an empty mask passes everything)");

    // Preview Model (or the selected GameObject)
    ImGui::SeparatorText("Preview");
    {
        std::string picked;
        std::string rejected;
        if (assetField(project, "mask_preview", data.previewModel, !data.previewModel.empty() && !fileExists(project, data.previewModel),
                       FILE_DIALOG_MODEL | FILE_DIALOG_PREFAB, isModelOrPrefab, true, picked, &rejected)){
            anim::AvatarMaskData edited = data;
            edited.previewModel = picked;
            commit(edited, "");
            state.useSelection = false;
        }
        if (!rejected.empty()) state.error = rejected;
        ImGui::Checkbox("Use Selection##mask_use_selection", &state.useSelection);
        ImGui::SetItemTooltip("Build the tree from the GameObject selected in the Hierarchy instead of the Preview Model");
    }
    // the preview hierarchy: rebuilt when its source changes (and once a second for a selection)
    const uint32_t sceneId = project->getSelectedSceneId();
    const std::vector<Entity> selection = project->getSelectedEntities(sceneId);
    SceneProject* sceneProject = project->getScene(sceneId);
    std::string key;
    if (state.useSelection && sceneProject && sceneProject->scene && selection.size() == 1){
        key = "entity:" + std::to_string(sceneId) + ":" + std::to_string(selection[0]) + ":" + std::to_string(sceneProject->scene->hierarchyRevision());
    }else if (!data.previewModel.empty()){
        key = "model:" + data.previewModel;
    }
    if (key != state.previewKey){
        state.previewKey = key;
        state.preview = animlayersui::PreviewHierarchy();
        if (key.rfind("entity:", 0) == 0){
            animlayersui::previewHierarchyFromEntity(sceneProject->scene, selection[0], state.preview);
        }else if (!key.empty()){
            animlayersui::previewHierarchyFromModel(project, data.previewModel, state.preview);
        }
    }
    const animlayersui::PreviewHierarchy& preview = state.preview;
    if (key.empty()){
        ImGui::TextDisabled(state.useSelection ? "Select one GameObject in the Hierarchy." : "Set a Preview Model to see its bones.");
    }else if (!preview.registry){
        ImGui::TextColored(Theme::Colors::WarningText, "%s", preview.error.c_str());
    }

    // the mask as the runtime builds it (what the tree's effective weights come from)
    if (state.maskBuiltPath != path || state.maskBuiltRevision != document->revision() || !state.maskBuilt){
        state.maskBuilt = anim::AvatarMask::build(data);
        state.maskBuiltPath = path;
        state.maskBuiltRevision = document->revision();
    }

    // Humanoid
    ImGui::SeparatorText("Humanoid");
    if (preview.registry && !preview.humanoid.valid){
        ImGui::TextDisabled(ICON_FA_CIRCLE_INFO " Not humanoid: the preview has no humanoid rig (%s)", preview.humanoid.error.c_str());
    }
    {
        const float scale = ImGui::GetFontSize();
        const ImVec2 size(scale * 9.0f, scale * 12.0f);
        const float offset = std::max(0.0f, (ImGui::GetContentRegionAvail().x - size.x) * 0.5f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##mask_figure", size);
        UIProbe::item("mask.figure");
        const bool hovered = ImGui::IsItemHovered();
        int hoveredPart = -1;
        if (hovered){
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            hoveredPart = animlayersui::figurePartAt((mouse.x - origin.x) / size.x, (mouse.y - origin.y) / size.y);
        }
        ImDrawList* draw = ImGui::GetWindowDrawList();
        for (const animlayersui::FigureShape& shape : animlayersui::figureShapes()){
            const ImVec2 a(origin.x + shape.x0 * size.x, origin.y + shape.y0 * size.y);
            const ImVec2 b(origin.x + shape.x1 * size.x, origin.y + shape.y1 * size.y);
            const float weight = data.hasHumanoid ? data.partWeight(shape.part) : data.defaultWeight;
            const ImU32 fill = weightColor(weight, data.hasHumanoid ? 1.0f : 0.35f);
            const ImU32 outline = (int)shape.part == hoveredPart ? IM_COL32(255, 255, 255, 255) : IM_COL32(20, 20, 20, 200);
            if (shape.disc){
                const ImVec2 centre((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
                draw->AddEllipseFilled(centre, ImVec2((b.x - a.x) * 0.5f, (b.y - a.y) * 0.5f), fill);
                draw->AddEllipse(centre, ImVec2((b.x - a.x) * 0.5f, (b.y - a.y) * 0.5f), outline, 0.0f, 0, 1.5f);
            }else{
                draw->AddRectFilled(a, b, fill, scale * 0.3f);
                draw->AddRect(a, b, outline, scale * 0.3f, 0, 1.5f);
            }
        }
        if (hoveredPart >= 0){
            const anim::AvatarMaskPart part = (anim::AvatarMaskPart)hoveredPart;
            ImGui::SetTooltip("%s: %.2f%s\nClick to toggle it (1 / 0)", animlayersui::partLabel(part), data.partWeight(part),
                              data.parts[(size_t)hoveredPart] ? "" : " (Default)");
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)){
                state.selectedPart = hoveredPart;
                commit(animlayersui::togglePart(data, part), "");
            }
        }
    }
    if (state.selectedPart >= 0 && state.selectedPart < anim::AVATAR_MASK_PART_COUNT){
        const anim::AvatarMaskPart part = (anim::AvatarMaskPart)state.selectedPart;
        float weight = data.partWeight(part);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        const std::string format = std::string(animlayersui::partLabel(part)) + " %.2f";
        if (ImGui::SliderFloat("##mask_part_weight", &weight, 0.0f, 1.0f, format.c_str())){
            anim::AvatarMaskData edited = data;
            edited.hasHumanoid = true;
            edited.parts[(size_t)part] = std::clamp(weight, 0.0f, 1.0f);
            commit(edited, "mask-part-" + std::to_string(state.selectedPart));
        }
        ImGui::SetItemTooltip("The weight of the part last clicked (a fraction blends it)");
    }
    if (data.hasHumanoid && ImGui::SmallButton("Remove Humanoid Parts##mask_no_humanoid")){
        anim::AvatarMaskData edited = data;
        edited.hasHumanoid = false;
        for (auto& weight : edited.parts) weight.reset();
        commit(edited, "");
    }

    // Transforms
    ImGui::SeparatorText("Transforms");
    UIProbe::item("mask.transforms");
    if (preview.registry && !preview.paths.empty()){
        const std::vector<Entity> entries = state.maskBuilt->bindTransforms(preview.registry.get(), preview.root);
        int hiddenBelow = -1;
        for (size_t i = 0; i < preview.paths.size(); i++){
            const std::string& nodePath = preview.paths[i];
            const int depth = preview.depths[i];
            if (hiddenBelow >= 0){
                if (depth > hiddenBelow) continue;
                hiddenBelow = -1;
            }
            const bool hasChildren = i + 1 < preview.paths.size() && preview.depths[i + 1] > depth;
            const bool collapsed = state.collapsed.count(nodePath) != 0;
            if (collapsed) hiddenBelow = depth;
            ImGui::PushID((int)i);
            ImGui::Indent((float)depth * ImGui::GetFontSize() * 0.8f + 1.0f);
            if (hasChildren){
                if (ImGui::SmallButton(collapsed ? ICON_FA_CARET_RIGHT : ICON_FA_CARET_DOWN)){
                    if (collapsed) state.collapsed.erase(nodePath);
                    else state.collapsed.insert(nodePath);
                }
                ImGui::SameLine();
            }
            const int entry = data.findTransform(nodePath);
            bool on = entry >= 0 && data.transforms[(size_t)entry].weight > 0.0f;
            if (ImGui::Checkbox("##node", &on)){
                // Shift: the whole subtree
                commit(animlayersui::setTransform(data, nodePath, on, ImGui::GetIO().KeyShift, preview.paths), "");
            }
            ImGui::SetItemTooltip("%s\nShift-click: the whole subtree", nodePath.c_str());
            ImGui::SameLine();
            // the effective weight as the player sees it (transform entry, body part or default)
            const anim::AvatarMaskWeight effective = state.maskBuilt->weightOf(preview.registry.get(), preview.root, preview.entities[i], entries,
                                                                                preview.humanoid.valid ? &preview.humanoid : nullptr);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 dot = ImGui::GetCursorScreenPos();
            const float r = ImGui::GetFontSize() * 0.25f;
            draw->AddCircleFilled(ImVec2(dot.x + r, dot.y + ImGui::GetFrameHeight() * 0.5f), r, weightColor(effective.weight));
            ImGui::Dummy(ImVec2(r * 2.0f, ImGui::GetFrameHeight()));
            ImGui::SetItemTooltip("%.2f (%s)", effective.weight, effective.ruleName().c_str());
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(preview.registry->getEntityName(preview.entities[i]).c_str());
            if (entry >= 0){
                const anim::AvatarMaskTransform& transform = data.transforms[(size_t)entry];
                ImGui::SameLine();
                float weight = transform.weight;
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
                if (ImGui::DragFloat("##weight", &weight, 0.01f, 0.0f, 1.0f, "%.2f")){
                    anim::AvatarMaskData edited = data;
                    edited.transforms[(size_t)entry].weight = std::clamp(weight, 0.0f, 1.0f);
                    commit(edited, "mask-transform-" + nodePath);
                }
                ImGui::SameLine();
                bool children = transform.children;
                if (ImGui::Checkbox("children##children", &children)){
                    anim::AvatarMaskData edited = data;
                    edited.transforms[(size_t)entry].children = children;
                    commit(edited, "");
                }
                ImGui::SetItemTooltip("The entry also covers the bones below it");
            }
            ImGui::Unindent((float)depth * ImGui::GetFontSize() * 0.8f + 1.0f);
            ImGui::PopID();
        }
    }
    // entries the preview does not have (another rig, or no preview)
    for (size_t t = 0; t < data.transforms.size(); t++){
        const anim::AvatarMaskTransform& transform = data.transforms[t];
        if (preview.registry && std::find(preview.paths.begin(), preview.paths.end(), transform.path) != preview.paths.end()) continue;
        ImGui::PushID((int)t + 100000);
        if (ImGui::SmallButton(ICON_FA_XMARK)){
            anim::AvatarMaskData edited = data;
            edited.transforms.erase(edited.transforms.begin() + (long)t);
            commit(edited, "");
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s  %.2f%s", transform.path.empty() ? "(the root)" : transform.path.c_str(), transform.weight,
                            transform.children ? " +children" : "");
        if (preview.registry) ImGui::SetItemTooltip("Not in the preview hierarchy");
        ImGui::PopID();
    }

    // Users
    ImGui::SeparatorText("Users");
    InspectorLayerState::UsersEntry& usersEntry = state.users["mask:" + path];
    if (usersEntry.at < 0.0 || ImGui::GetTime() - usersEntry.at > RETRY_SECONDS){
        usersEntry.users = findMaskUsers(project, path);
        usersEntry.at = ImGui::GetTime();
    }
    if (usersEntry.users.empty()) ImGui::TextDisabled("No player or controller uses it in open scenes and loaded prefabs");
    size_t listed = 0;
    for (const AnimAssetUser& user : usersEntry.users){
        if (listed++ >= MAX_ROWS){
            ImGui::TextDisabled("... and %zu more", usersEntry.users.size() - MAX_ROWS);
            break;
        }
        ImGui::PushID((int)listed);
        if (!user.controllerFile.empty()){
            ImGui::BulletText("%s, layer %d", user.controllerFile.c_str(), user.layer);
        }else if (user.prefabAsset){
            ImGui::BulletText("%s (prefab asset), layer %d", fs::path(user.prefabPath).filename().string().c_str(), user.layer);
        }else{
            SceneProject* userScene = project->getScene(user.sceneId);
            std::string text = (userScene ? userScene->name : std::string("?")) + ": ";
            if (userScene && userScene->scene && userScene->scene->isEntityCreated(user.entity)){
                text += userScene->scene->getEntityName(user.entity);
                if (const AnimationPlayerComponent* player = userScene->scene->findComponent<AnimationPlayerComponent>(user.entity)){
                    const std::vector<AnimationPlayerLayer> layers = anim::playerLayers(*player);
                    if (user.layer >= 0 && (size_t)user.layer < layers.size()) text += " (layer " + layers[(size_t)user.layer].name + ")";
                }
            }
            ImGui::Bullet();
            if (userScene && userScene->id == project->getSelectedSceneId() && userScene->scene->isEntityCreated(user.entity)){
                if (ImGui::Selectable(text.c_str())){
                    project->replaceSelectedEntities(user.sceneId, {user.entity});
                    project->setSelectedSceneForProperties(user.sceneId);
                    project->clearInspectedAsset();
                }
            }else{
                ImGui::TextUnformatted(text.c_str());
            }
        }
        ImGui::PopID();
    }

    // warnings of the last load, and the edit errors
    if (!document->loadWarnings().empty()){
        ImGui::SeparatorText("Warnings");
        for (const ClipWarning& warning : document->loadWarnings()){
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
            ImGui::TextWrapped("%s%s%s", warning.path.c_str(), warning.path.empty() ? "" : ": ", warning.message.c_str());
            ImGui::PopStyleColor();
        }
    }
    if (!state.error.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("%s", state.error.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton(ICON_FA_XMARK " Dismiss##mask_error")) state.error.clear();
    }
}

// ---- the Animator Controller asset view ----

void editor::InspectorWindow::drawControllerAsset(const fs::path& asset){
    InspectorLayerState& state = layerState();
    drawAssetHeader(ICON_FA_DIAGRAM_PROJECT, asset, "Animator Controller");
    const std::string path = project->normalizeToProjectRelative(asset).generic_string();
    const AnimationClipAppHooks& hooks = animationClipAppHooks();
    ImGui::BeginDisabled(!hooks.openAnimator);
    if (ImGui::Button("Open in Animator##controller_open", ImVec2(ImGui::GetContentRegionAvail().x, 0)) && hooks.openAnimator){
        hooks.openAnimator(project, path, 0, NULL_ENTITY);
    }
    ImGui::EndDisabled();
    UIProbe::item("controller.open");
    std::string error;
    std::shared_ptr<AnimatorControllerDocument> document = controllerDocument(project, state, path, &error);
    if (!document){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("%s", error.c_str());
        ImGui::PopStyleColor();
        return;
    }
    const anim::ControllerData& data = document->data();
    ImGui::SeparatorText("Parameters");
    if (data.parameters.empty()) ImGui::TextDisabled("None");
    for (const anim::ControllerParameter& parameter : data.parameters){
        ImGui::BulletText("%s (%s)", parameter.name.c_str(), anim::parameterTypeName(parameter.type));
    }
    ImGui::SeparatorText("Layers");
    for (size_t l = 0; l < data.layers.size(); l++){
        const anim::ControllerLayer& layer = data.layers[l];
        const int defaultState = layer.defaultStateIndex();
        ImGui::BulletText("%s: %zu state%s, %zu transition%s%s%s", layer.name.c_str(), layer.states.size(), layer.states.size() == 1 ? "" : "s",
                          layer.transitions.size(), layer.transitions.size() == 1 ? "" : "s", defaultState >= 0 ? ", default " : "",
                          defaultState >= 0 ? layer.states[(size_t)defaultState].name.c_str() : "");
    }
    const std::vector<ClipWarning>& warnings = controllerWarnings(project, state, path, *document);
    ImGui::SeparatorText("Problems");
    if (warnings.empty()){
        ImGui::TextColored(OK_COLOR, ICON_FA_CHECK " None");
    }
    size_t shown = 0;
    for (const ClipWarning& warning : warnings){
        if (shown++ >= MAX_ROWS * 2){
            ImGui::TextDisabled("... and %zu more", warnings.size() - MAX_ROWS * 2);
            break;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
        ImGui::TextWrapped("%s%s%s", warning.path.c_str(), warning.path.empty() ? "" : ": ", warning.message.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::SeparatorText("Users");
    InspectorLayerState::UsersEntry& usersEntry = state.users["controller:" + path];
    if (usersEntry.at < 0.0 || ImGui::GetTime() - usersEntry.at > RETRY_SECONDS){
        usersEntry.users = findControllerUsers(project, path);
        usersEntry.at = ImGui::GetTime();
    }
    if (usersEntry.users.empty()) ImGui::TextDisabled("No Animation Player uses it in open scenes and loaded prefabs");
    for (const AnimAssetUser& user : usersEntry.users){
        SceneProject* userScene = project->getScene(user.sceneId);
        if (user.prefabAsset){
            ImGui::BulletText("%s (prefab asset)", fs::path(user.prefabPath).filename().string().c_str());
        }else if (userScene && userScene->scene && userScene->scene->isEntityCreated(user.entity)){
            ImGui::BulletText("%s: %s", userScene->name.c_str(), userScene->scene->getEntityName(user.entity).c_str());
        }
    }
}
