// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector of the Animator (docs/specs/S21-animation.md R22, R25, R27): the Animator
// component (its reflected rows, then Open / New controller, the avatar, the live states while
// playing, problems), and the Animator assets selected in the Project window: an Animator
// Controller (a summary and its diagnostics), an Avatar Mask (the body-part figure and the
// transform paths) and an Animation Montage (clip, slot, blends, the section bar, events).

#include "InspectorWindow.h"

#include "Backend.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimatorAssetDocument.h"
#include "animation/AnimatorEditorHooks.h"
#include "animation/AnimatorInstance.h"
#include "animation/CompiledController.h"
#include "animation/ModelSubClips.h"
#include "command/CommandHandle.h"
#include "command/type/PropertyCmd.h"
#include "component/AnimationPlayerComponent.h"
#include "component/AnimatorComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "external/IconsFontAwesome6.h"
#include "pool/AnimationClipPool.h"
#include "pool/AnimatorAssetPool.h"
#include "pose/PoseRigProvider.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/UIProbe.h"
#include "util/Util.h"
#include "Out.h"
#include "Scene.h"
#include "Theme.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::anim;
namespace fs = std::filesystem;

namespace{

    // an asset edit in progress (a drag): shown at once, written as one step when it ends
    struct PendingAssetEdit{
        std::shared_ptr<AnimatorAssetDocument> document;
        std::optional<AvatarMaskDef> mask;
        std::optional<MontageDef> montage;
        std::string undo;
    };
    PendingAssetEdit& pendingEdit(){
        static PendingAssetEdit edit;
        return edit;
    }

    void flushPending(){
        PendingAssetEdit& edit = pendingEdit();
        if (!edit.document || ImGui::IsAnyItemActive()) return;
        std::string error;
        bool ok = true;
        if (edit.mask) ok = edit.document->commit(*edit.mask, edit.undo, std::string(), &error);
        else if (edit.montage) ok = edit.document->commit(*edit.montage, edit.undo, std::string(), &error);
        if (!ok) Out::error("%s", error.c_str());
        edit = PendingAssetEdit();
    }

    AvatarMaskDef maskOf(const std::shared_ptr<AnimatorAssetDocument>& document){
        const PendingAssetEdit& edit = pendingEdit();
        if (edit.document == document && edit.mask) return *edit.mask;
        return document->mask();
    }

    MontageDef montageOf(const std::shared_ptr<AnimatorAssetDocument>& document){
        const PendingAssetEdit& edit = pendingEdit();
        if (edit.document == document && edit.montage) return *edit.montage;
        return document->montage();
    }

    void stageMask(const std::shared_ptr<AnimatorAssetDocument>& document, const AvatarMaskDef& mask, const std::string& undo){
        PendingAssetEdit& edit = pendingEdit();
        if (edit.document && edit.document != document) flushPending();
        edit.document = document;
        edit.mask = mask;
        edit.montage.reset();
        edit.undo = undo;
    }

    void stageMontage(const std::shared_ptr<AnimatorAssetDocument>& document, const MontageDef& montage, const std::string& undo){
        PendingAssetEdit& edit = pendingEdit();
        if (edit.document && edit.document != document) flushPending();
        edit.document = document;
        edit.montage = montage;
        edit.mask.reset();
        edit.undo = undo;
    }

    void errorText(const std::string& text){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
    }

    void warningText(const std::string& text){
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", text.c_str());
    }

    bool isAncestorOrSelf(Scene* scene, Entity ancestor, Entity entity){
        for (int guard = 0; entity != NULL_ENTITY && guard < 4096; guard++){
            if (entity == ancestor) return true;
            const Transform* transform = scene->findComponent<Transform>(entity);
            entity = transform ? transform->parent : NULL_ENTITY;
        }
        return false;
    }

    // the Animator's model: on it, else the first model GameObject below it
    Entity animatorModel(Scene* scene, Entity entity){
        if (scene->findComponent<ModelComponent>(entity)) return entity;
        auto models = scene->getComponentArray<ModelComponent>();
        for (size_t i = 0; i < models->size(); i++){
            const Entity model = models->getEntity(i);
            if (isAncestorOrSelf(scene, entity, model)) return model;
        }
        return NULL_ENTITY;
    }

    std::string uniqueAssetPath(Project* project, const std::string& stem, const char* extension){
        std::string clean = stem.empty() ? std::string("New") : stem;
        for (char& c : clean){
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        }
        const fs::path assets = project->getAssetsPath();
        fs::path candidate = assets / (clean + extension);
        for (int n = 1; fs::exists(candidate); n++) candidate = assets / (clean + "_" + std::to_string(n) + extension);
        return candidate.string();
    }

    // the Avatar Mask figure: a part's rectangle in a 0..1 box
    struct FigurePart{
        AvatarBodyPart part;
        float x0, y0, x1, y1;
        bool round;
    };
    const FigurePart kFigure[] = {
        {AvatarBodyPart::Head, 0.42f, 0.02f, 0.58f, 0.16f, true},
        {AvatarBodyPart::Body, 0.40f, 0.17f, 0.60f, 0.50f, false},
        {AvatarBodyPart::LeftArm, 0.61f, 0.18f, 0.69f, 0.44f, false},
        {AvatarBodyPart::RightArm, 0.31f, 0.18f, 0.39f, 0.44f, false},
        {AvatarBodyPart::LeftHand, 0.61f, 0.45f, 0.70f, 0.53f, true},
        {AvatarBodyPart::RightHand, 0.30f, 0.45f, 0.39f, 0.53f, true},
        {AvatarBodyPart::LeftLeg, 0.51f, 0.51f, 0.59f, 0.90f, false},
        {AvatarBodyPart::RightLeg, 0.41f, 0.51f, 0.49f, 0.90f, false},
        {AvatarBodyPart::Root, 0.44f, 0.92f, 0.56f, 0.98f, true},
    };

    const char* bodyPartLabel(AvatarBodyPart part){
        switch (part){
            case AvatarBodyPart::Root: return "Root (root motion)";
            case AvatarBodyPart::Body: return "Body";
            case AvatarBodyPart::Head: return "Head";
            case AvatarBodyPart::LeftArm: return "Left Arm";
            case AvatarBodyPart::RightArm: return "Right Arm";
            case AvatarBodyPart::LeftHand: return "Left Hand";
            case AvatarBodyPart::RightHand: return "Right Hand";
            case AvatarBodyPart::LeftLeg: return "Left Leg";
            case AvatarBodyPart::RightLeg: return "Right Leg";
            default: return "";
        }
    }

    // the model a mask's transform list was last filled from (per mask document)
    std::map<std::string, std::string>& maskModels(){
        static std::map<std::string, std::string> models;
        return models;
    }

}

// ---------------------------------------------------------------- the component

void editor::InspectorWindow::drawAnimatorComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    drawDefaultComponent(cpType, sceneProject, entities);
    if (entities.size() != 1) return;
    Scene* scene = sceneProject->scene;
    const Entity entity = entities[0];
    AnimatorComponent* animator = scene->findComponent<AnimatorComponent>(entity);
    if (!animator) return;
    const AnimatorEditorHooks& hooks = animatorEditorHooks();
    const bool stopped = sceneProject->playState == ScenePlayState::STOPPED;
    // the edits below are no component edits: they stay enabled while the rows are read-only
    const bool disabled = ImGui::GetItemFlags() & ImGuiItemFlags_Disabled;
    if (disabled) ImGui::EndDisabled();

    // the controller: open it, or make one
    const float width = ImGui::GetContentRegionAvail().x;
    const float half = (width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImGui::BeginDisabled(!hooks.openController || animator->controller.path.empty());
    if (ImGui::Button(ICON_FA_DIAGRAM_PROJECT " Open Animator", ImVec2(half, 0))){
        hooks.openController(project, sceneProject->id, entity, animator->controller.path);
    }
    ImGui::EndDisabled();
    UIProbe::item("animator.openWindow");
    ImGui::SetItemTooltip("Edit the controller in Window > Animation > Animator (it follows this Animator)");
    ImGui::SameLine();
    AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(entity);
    ImGui::BeginDisabled(!stopped);
    if (ImGui::Button(player && !player->clips.empty() ? ICON_FA_PLUS " New from Player" : ICON_FA_PLUS " New Controller", ImVec2(half, 0))){
        // a .controller in Assets (one project step) assigned to the Animator (one scene step)
        std::string error;
        const std::string path = uniqueAssetPath(project, scene->getEntityName(entity), ".controller");
        std::shared_ptr<AnimatorAssetDocument> document =
            AnimatorAssetDocument::create(project, path, AnimatorAssetDocument::Kind::Controller, "Create Animator Controller", &error);
        if (document && player && !player->clips.empty()){
            ControllerDef def = document->controller();
            appendPlayerStates(def, *player);
            if (!document->commit(def, "States from Animation Player", std::string(), &error)) document.reset();
        }
        if (!document){
            Backend::getApp().registerAlert("Create Animator Controller", error);
        }else{
            const AssetRef ref = project->makeAssetRef(document->assetPath(), AssetPathBase::Assets);
            CommandHandle::get(sceneProject->id)->addCommandNoMerge(
                new PropertyCmd<AssetRef>(project, sceneProject->id, entity, cpType, "controller", ref));
            if (hooks.openController) hooks.openController(project, sceneProject->id, entity, document->assetPath());
        }
    }
    ImGui::EndDisabled();
    UIProbe::item("animator.newController");
    ImGui::SetItemTooltip(player && !player->clips.empty()
        ? "Create an Animator Controller with a state per clip of this GameObject's Animation Player, and assign it"
        : "Create an empty Animator Controller in Assets and assign it");

    // the avatar: the model's humanoid map (retargeting, humanoid masks, foot IK)
    const Entity model = animatorModel(scene, entity);
    ImGui::SeparatorText("Avatar");
    if (model == NULL_ENTITY){
        ImGui::TextDisabled("No model on or below this GameObject: transform clips only.");
    }else{
        const ModelComponent& modelComponent = scene->getComponent<ModelComponent>(model);
        std::shared_ptr<const PoseRigInfo> rig;
        if (!modelComponent.filename.empty()) rig = PoseRigProvider::forModelFile(project, project->resolveAssetPath(modelComponent.filename));
        if (rig && rig->rig){
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), ICON_FA_CHECK);
            ImGui::SameLine();
            ImGui::TextWrapped("Humanoid (%s)", fs::path(modelComponent.filename).filename().string().c_str());
        }else{
            ImGui::TextDisabled("Generic%s%s", rig && !rig->error.empty() ? ": " : "", rig ? rig->error.c_str() : "");
        }
        ImGui::BeginDisabled(!hooks.configureAvatar || !stopped);
        if (ImGui::Button(ICON_FA_PERSON " Configure Avatar...", ImVec2(width, 0))){
            hooks.configureAvatar(project, sceneProject->id, model);
        }
        ImGui::EndDisabled();
        UIProbe::item("animator.configureAvatar");
        ImGui::SetItemTooltip("Map the model's bones to the humanoid slots (the model's .humanoid file) in the Pose Animation window");
    }

    // problems and the running states
    if (animator->runtime){
        const AnimatorInstance& runtime = *animator->runtime;
        if (!runtime.controllerError.empty()) warningText(runtime.controllerError);
        if (!runtime.bindError.empty()) warningText(runtime.bindError);
        if (runtime.controller){
            for (const ClipWarning& warning : runtime.controller->diagnostics) warningText(warning.path.empty() ? warning.message : warning.path + ": " + warning.message);
        }
        if (!stopped && runtime.controller && runtime.bound){
            ImGui::SeparatorText("State");
            for (size_t l = 0; l < runtime.layers.size() && l < runtime.controller->layers.size(); l++){
                const AnimatorInstance::StateInfo current = runtime.currentState((int)l);
                std::string line = current.fullPath + "  " + std::to_string((int)std::lround(current.normalizedTime * 100.0f)) + "%";
                if (runtime.isInTransition((int)l)){
                    line += "  " ICON_FA_ARROW_RIGHT " " + runtime.nextState((int)l).fullPath;
                }
                ImGui::TextUnformatted(line.c_str());
            }
            if (const AnimatorMontageRun* montage = runtime.activeMontage()){
                ImGui::Text(ICON_FA_FILM " %s  %.2f s", fs::path(montage->path).filename().string().c_str(), montage->time);
            }
        }
    }else if (!animator->controller.path.empty() && stopped){
        std::string error;
        std::shared_ptr<const CompiledController> compiled = AnimatorAssetPool::controller(animator->controller.path, &error);
        if (!compiled) warningText(error);
        else for (const ClipWarning& warning : compiled->diagnostics) warningText(warning.path.empty() ? warning.message : warning.path + ": " + warning.message);
    }
    if (disabled) ImGui::BeginDisabled(true);
}

// ---------------------------------------------------------------- the assets

bool editor::InspectorWindow::drawAnimatorAsset(const fs::path& asset){
    flushPending();
    AnimatorAssetDocument::Kind kind;
    if (!AnimatorAssetDocument::kindOf(asset.string(), kind)) return false;
    std::string error;
    std::shared_ptr<AnimatorAssetDocument> document = AnimatorAssetDocument::open(project, asset.string(), &error);
    const char* icon = kind == AnimatorAssetDocument::Kind::Controller ? ICON_FA_DIAGRAM_PROJECT
                     : kind == AnimatorAssetDocument::Kind::Mask ? ICON_FA_PERSON : ICON_FA_FILM;
    drawAssetHeader(icon, asset, AnimatorAssetDocument::kindName(kind));
    if (!document){
        errorText(error);
        return true;
    }
    if (!document->loadError().empty()){
        errorText(document->loadError());
        return true;
    }
    for (const ClipWarning& warning : document->warnings()) warningText(warning.path.empty() ? warning.message : warning.path + ": " + warning.message);
    if (kind == AnimatorAssetDocument::Kind::Controller) drawControllerAsset(document);
    else if (kind == AnimatorAssetDocument::Kind::Mask) drawMaskAsset(document);
    else drawMontageAsset(document);
    flushPending();
    return true;
}

void editor::InspectorWindow::drawControllerAsset(const std::shared_ptr<AnimatorAssetDocument>& document){
    const ControllerDef& def = document->controller();
    const AnimatorEditorHooks& hooks = animatorEditorHooks();
    ImGui::BeginDisabled(!hooks.openController);
    if (ImGui::Button(ICON_FA_DIAGRAM_PROJECT " Open in Animator", ImVec2(ImGui::GetContentRegionAvail().x, 0))){
        hooks.openController(project, 0, NULL_ENTITY, document->assetPath());
    }
    ImGui::EndDisabled();
    UIProbe::item("asset.controller.open");

    ImGui::SeparatorText("Parameters");
    if (def.parameters.empty()) ImGui::TextDisabled("(none)");
    for (const ParameterDef& p : def.parameters){
        ImGui::BulletText("%s  %s", p.name.c_str(), parameterTypeName(p.type));
    }
    ImGui::SeparatorText("Layers");
    std::function<size_t(const StateMachineDef&)> count = [&](const StateMachineDef& m){
        size_t n = m.states.size();
        for (const StateMachineDef& sub : m.subMachines) n += count(sub);
        return n;
    };
    for (const LayerDef& layer : def.layers){
        ImGui::BulletText("%s  %zu state(s)  %s%s%s", layer.name.c_str(), count(layer.machine), layerBlendingName(layer.blending),
                          layer.mask.path.empty() ? "" : "  mask ", layer.mask.path.c_str());
    }
    // the compiler's diagnostics (unknown parameters, missing targets, unreachable states)
    std::shared_ptr<const CompiledController> compiled = compileController(def);
    if (compiled && !compiled->diagnostics.empty()){
        ImGui::SeparatorText("Problems");
        for (const ClipWarning& warning : compiled->diagnostics) warningText(warning.path.empty() ? warning.message : warning.path + ": " + warning.message);
    }
}

void editor::InspectorWindow::drawMaskAsset(const std::shared_ptr<AnimatorAssetDocument>& document){
    AvatarMaskDef mask = maskOf(document);
    bool changed = false;
    std::string undo = "Edit Avatar Mask";

    // the humanoid figure: a click toggles a body part (green: animated, red: masked out)
    ImGui::SeparatorText("Humanoid");
    const float side = std::min(ImGui::GetContentRegionAvail().x, Theme::dpi(220.0f));
    const ImVec2 origin(ImGui::GetCursorScreenPos().x + (ImGui::GetContentRegionAvail().x - side) * 0.5f, ImGui::GetCursorScreenPos().y);
    ImDrawList* list = ImGui::GetWindowDrawList();
    list->AddRectFilled(origin, ImVec2(origin.x + side, origin.y + side), IM_COL32(30, 30, 34, 255), 4.0f);
    int hoveredPart = -1;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (const FigurePart& f : kFigure){
        const int index = (int)f.part;
        const ImVec2 a(origin.x + f.x0 * side, origin.y + f.y0 * side);
        const ImVec2 b(origin.x + f.x1 * side, origin.y + f.y1 * side);
        const bool on = mask.bodyParts[index];
        const bool hovered = mouse.x >= a.x && mouse.x <= b.x && mouse.y >= a.y && mouse.y <= b.y && ImGui::IsWindowHovered();
        if (hovered) hoveredPart = index;
        ImU32 color = on ? IM_COL32(70, 170, 80, 255) : IM_COL32(170, 60, 60, 255);
        if (hovered) color = on ? IM_COL32(100, 200, 110, 255) : IM_COL32(200, 90, 90, 255);
        if (f.round) list->AddEllipseFilled(ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), ImVec2((b.x - a.x) * 0.5f, (b.y - a.y) * 0.5f), color);
        else list->AddRectFilled(a, b, color, 3.0f);
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##mask_figure", ImVec2(side, side));
    UIProbe::item("asset.mask.figure");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && hoveredPart >= 0){
        mask.bodyParts[hoveredPart] = !mask.bodyParts[hoveredPart];
        changed = true;
        undo = std::string(mask.bodyParts[hoveredPart] ? "Include " : "Mask Out ") + bodyPartLabel((AvatarBodyPart)hoveredPart);
    }
    if (hoveredPart >= 0) ImGui::SetItemTooltip("%s: %s (click to toggle)", bodyPartLabel((AvatarBodyPart)hoveredPart),
                                               mask.bodyParts[hoveredPart] ? "animated" : "masked out");
    // the same as checkboxes (keyboard, tests)
    if (ImGui::BeginTable("##mask_parts", 3)){
        for (int i = 0; i < AVATAR_BODY_PART_COUNT; i++){
            ImGui::TableNextColumn();
            bool on = mask.bodyParts[i];
            if (ImGui::Checkbox(bodyPartLabel((AvatarBodyPart)i), &on)){
                mask.bodyParts[i] = on;
                changed = true;
                undo = std::string(on ? "Include " : "Mask Out ") + bodyPartLabel((AvatarBodyPart)i);
            }
        }
        ImGui::EndTable();
    }

    // the transform paths: every bone of a model, ticked and weighted
    ImGui::SeparatorText("Transforms");
    ImGui::TextDisabled("Paths below the Animator's GameObject; a listed path counts with its weight, the others follow the body parts.");
    std::string& modelPath = maskModels()[document->projectPath()];
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "%s", modelPath.c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##mask_model", "Drop a model to list its bones", buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue)){
        modelPath = buffer;
    }
    if (ImGui::BeginDragDropTarget()){
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
            std::vector<std::string> dropped = Util::getStringsFromPayload(payload);
            if (!dropped.empty() && Util::isModelFile(dropped[0]) && project->isInsideAssetsPath(dropped[0])){
                modelPath = project->normalizeToAssetsRelative(dropped[0]).generic_string();
            }
        }
        ImGui::EndDragDropTarget();
    }
    std::vector<std::string> paths;
    for (const MaskPathDef& entry : mask.paths) paths.push_back(entry.path);
    if (!modelPath.empty()){
        std::vector<ModelNodeInfo> nodes;
        std::string error;
        if (readModelFileNodes(modelPath, nodes, &error)){
            for (const ModelNodeInfo& node : nodes){
                if (!node.path.empty() && std::find(paths.begin(), paths.end(), node.path) == paths.end()) paths.push_back(node.path);
            }
        }else{
            warningText(error);
        }
    }
    if (ImGui::BeginTable("##mask_paths", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, std::min(Theme::dpi(320.0f), ImGui::GetFrameHeightWithSpacing() * (float)(paths.size() + 1))))){
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableSetupColumn("Path");
        ImGui::TableSetupColumn("Weight", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(70.0f));
        for (size_t i = 0; i < paths.size(); i++){
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            auto it = std::find_if(mask.paths.begin(), mask.paths.end(), [&](const MaskPathDef& e){ return e.path == paths[i]; });
            bool listed = it != mask.paths.end();
            if (ImGui::Checkbox("##on", &listed)){
                if (listed){
                    MaskPathDef entry;
                    entry.path = paths[i];
                    mask.paths.push_back(entry);
                }else{
                    mask.paths.erase(it);
                }
                changed = true;
                undo = (listed ? "Add Mask Path " : "Remove Mask Path ") + paths[i];
                it = std::find_if(mask.paths.begin(), mask.paths.end(), [&](const MaskPathDef& e){ return e.path == paths[i]; });
            }
            ImGui::TableNextColumn();
            // depth indent by the path's segments
            const size_t depth = (size_t)std::count(paths[i].begin(), paths[i].end(), '/');
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (float)depth * ImGui::GetStyle().IndentSpacing * 0.5f);
            const size_t slash = paths[i].rfind('/');
            ImGui::TextUnformatted(slash == std::string::npos ? paths[i].c_str() : paths[i].c_str() + slash + 1);
            ImGui::SetItemTooltip("%s", paths[i].c_str());
            ImGui::TableNextColumn();
            if (it != mask.paths.end()){
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderFloat("##weight", &it->weight, 0.0f, 1.0f, "%.2f")){
                    changed = true;
                    undo = "Mask Path Weight " + paths[i];
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (changed) stageMask(document, mask, undo);
}

void editor::InspectorWindow::drawMontageAsset(const std::shared_ptr<AnimatorAssetDocument>& document){
    MontageDef montage = montageOf(document);
    bool changed = false;
    std::string undo = "Edit Animation Montage";
    const float label = getLabelSize("Blend Out Ease");

    beginTable(ComponentType::AnimatorComponent, label, "montage_asset");
    propertyHeader("Clip");
    char clip[512];
    std::snprintf(clip, sizeof(clip), "%s", montage.clip.path.c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##montage_clip", clip, sizeof(clip), ImGuiInputTextFlags_EnterReturnsTrue)){
        montage.clip.path = clip;
        montage.clip.guid.clear();
        changed = true;
        undo = "Set Montage Clip";
    }
    if (ImGui::BeginDragDropTarget()){
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
            std::vector<std::string> dropped = Util::getStringsFromPayload(payload);
            if (!dropped.empty() && Util::isAnimationClipFile(dropped[0]) && project->isInsideAssetsPath(dropped[0])){
                montage.clip.path = project->normalizeToAssetsRelative(dropped[0]).generic_string();
                std::string text;
                montage.clip.guid = ProjectChecks::readFile(dropped[0], text) ? readClipGuid(text) : std::string();
                changed = true;
                undo = "Set Montage Clip";
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SetItemTooltip("An .anim or a model clip (model.gltf#Attack); drop an .anim from the Project window");
    propertyHeader("Slot");
    char slot[128];
    std::snprintf(slot, sizeof(slot), "%s", montage.slot.c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##montage_slot", slot, sizeof(slot), ImGuiInputTextFlags_EnterReturnsTrue) && slot[0]){
        montage.slot = slot;
        changed = true;
        undo = "Set Montage Slot";
    }
    ImGui::SetItemTooltip("The controller layer whose Montage Slot is this plays it (Default: the base layer)");
    propertyHeader("Rate");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::DragFloat("##montage_rate", &montage.rate, 0.01f, 0.01f, 10.0f, "%.2f")){ changed = true; undo = "Set Montage Rate"; }
    // the ease names a montage blend takes (AnimationClipIO's easeName table)
    static const char* const eases[] = {
        "linear", "step", "quad_in", "quad_out", "quad_in_out", "cubic_in", "cubic_out", "cubic_in_out",
        "quart_in", "quart_out", "quart_in_out", "quint_in", "quint_out", "quint_in_out", "sine_in", "sine_out", "sine_in_out",
        "expo_in", "expo_out", "expo_in_out", "circ_in", "circ_out", "circ_in_out", "elastic_in", "elastic_out", "elastic_in_out",
        "back_in", "back_out", "back_in_out", "bounce_in", "bounce_out", "bounce_in_out"};
    auto blendRows = [&](const char* title, MontageBlendDef& blend){
        propertyHeader(std::string(title) + " Time");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat((std::string("##") + title + "_time").c_str(), &blend.time, 0.01f, 0.0f, 10.0f, "%.2f s")){
            changed = true;
            undo = std::string("Set ") + title;
        }
        propertyHeader(std::string(title) + " Ease");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo((std::string("##") + title + "_ease").c_str(), blend.ease.c_str())){
            for (const char* name : eases){
                if (ImGui::Selectable(name, blend.ease == name)){
                    blend.ease = name;
                    changed = true;
                    undo = std::string("Set ") + title + " Ease";
                }
            }
            ImGui::EndCombo();
        }
    };
    blendRows("Blend In", montage.blendIn);
    blendRows("Blend Out", montage.blendOut);
    endTable();

    // the section bar: the clip's length split where each section starts
    float length = 0.0f;
    if (!montage.clip.path.empty()){
        std::string error;
        std::shared_ptr<const AnimationClip> loaded = AnimationClipPool::getOrLoad(montage.clip.path, &error);
        if (loaded) length = loaded->getLength();
        else warningText(error);
    }
    ImGui::SeparatorText("Sections");
    if (length > 0.0f && !montage.sections.empty()){
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float barWidth = ImGui::GetContentRegionAvail().x;
        const float barHeight = ImGui::GetFrameHeight();
        ImDrawList* list = ImGui::GetWindowDrawList();
        const ImU32 colors[] = {IM_COL32(70, 110, 170, 255), IM_COL32(90, 140, 90, 255), IM_COL32(150, 110, 60, 255), IM_COL32(130, 80, 140, 255)};
        for (size_t i = 0; i < montage.sections.size(); i++){
            const float start = std::clamp(montage.sections[i].start / length, 0.0f, 1.0f);
            const float end = i + 1 < montage.sections.size() ? std::clamp(montage.sections[i + 1].start / length, 0.0f, 1.0f) : 1.0f;
            const ImVec2 a(origin.x + start * barWidth, origin.y);
            const ImVec2 b(origin.x + end * barWidth, origin.y + barHeight);
            list->AddRectFilled(a, b, colors[i % 4]);
            list->PushClipRect(a, b, true);
            list->AddText(ImVec2(a.x + 4.0f, a.y + ImGui::GetStyle().FramePadding.y), IM_COL32(235, 235, 235, 255), montage.sections[i].name.c_str());
            list->PopClipRect();
        }
        for (const MontageEventDef& event : montage.events){
            const float x = origin.x + std::clamp(event.time / length, 0.0f, 1.0f) * barWidth;
            list->AddTriangleFilled(ImVec2(x - 4.0f, origin.y + barHeight + 7.0f), ImVec2(x + 4.0f, origin.y + barHeight + 7.0f),
                                    ImVec2(x, origin.y + barHeight), IM_COL32(230, 200, 80, 255));
        }
        ImGui::Dummy(ImVec2(barWidth, barHeight + 8.0f));
        ImGui::SetItemTooltip("%.2f s", length);
    }
    for (size_t i = 0; i < montage.sections.size(); i++){
        MontageSectionDef& section = montage.sections[i];
        ImGui::PushID((int)i);
        char name[128];
        std::snprintf(name, sizeof(name), "%s", section.name.c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.35f);
        if (ImGui::InputText("##name", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue) && name[0] && montage.findSection(name) < 0){
            const std::string from = section.name;
            for (MontageSectionDef& other : montage.sections) if (other.next == from) other.next = name;
            section.name = name;
            changed = true;
            undo = "Rename Section " + from;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.35f);
        ImGui::BeginDisabled(i == 0);
        if (ImGui::DragFloat("##start", &section.start, 0.01f, 0.0f, length > 0.0f ? length : 600.0f, "%.2f s")){
            changed = true;
            undo = "Move Section " + section.name;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::BeginCombo("##next", section.next.empty() ? "(end)" : section.next.c_str())){
            if (ImGui::Selectable("(end)", section.next.empty())){ section.next.clear(); changed = true; undo = "Set Next Section"; }
            for (const MontageSectionDef& other : montage.sections){
                if (ImGui::Selectable(other.name.c_str(), section.next == other.name)){
                    section.next = other.name;
                    changed = true;
                    undo = "Set Next Section";
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The section played after it (a loop when itself); (end): the montage blends out");
        ImGui::SameLine();
        ImGui::BeginDisabled(montage.sections.size() <= 1);
        if (ImGui::SmallButton(ICON_FA_XMARK)){
            const std::string removed = section.name;
            const std::string next = section.next == removed ? std::string() : section.next;
            montage.sections.erase(montage.sections.begin() + (long)i);
            for (MontageSectionDef& other : montage.sections) if (other.next == removed) other.next = next;
            if (!montage.sections.empty()) montage.sections.front().start = 0.0f;
            changed = true;
            undo = "Delete Section " + removed;
            ImGui::EndDisabled();
            ImGui::PopID();
            break;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    if (ImGui::Button(ICON_FA_PLUS " Section")){
        MontageSectionDef section;
        int n = (int)montage.sections.size();
        do { section.name = "Section " + std::to_string(++n); } while (montage.findSection(section.name) >= 0);
        const float last = montage.sections.empty() ? 0.0f : montage.sections.back().start;
        section.start = montage.sections.empty() ? 0.0f : (length > last ? (last + length) * 0.5f : last + 0.5f);
        if (!montage.sections.empty() && montage.sections.back().next.empty()) montage.sections.back().next = section.name;
        montage.sections.push_back(section);
        changed = true;
        undo = "Add Section " + section.name;
    }

    ImGui::SeparatorText("Events");
    for (size_t i = 0; i < montage.events.size(); i++){
        MontageEventDef& event = montage.events[i];
        ImGui::PushID((int)(1000 + i));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.3f);
        if (ImGui::DragFloat("##time", &event.time, 0.01f, 0.0f, length > 0.0f ? length : 600.0f, "%.2f s")){
            changed = true;
            undo = "Move Montage Event";
        }
        ImGui::SameLine();
        char function[128];
        std::snprintf(function, sizeof(function), "%s", event.function.c_str());
        ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::InputText("##function", function, sizeof(function), ImGuiInputTextFlags_EnterReturnsTrue) && function[0]){
            event.function = function;
            changed = true;
            undo = "Set Montage Event Function";
        }
        if (!event.args.empty()) ImGui::SetItemTooltip("%zu argument(s) (edit_montage sets them)", event.args.size());
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_XMARK)){
            montage.events.erase(montage.events.begin() + (long)i);
            changed = true;
            undo = "Delete Montage Event";
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::Button(ICON_FA_PLUS " Event")){
        MontageEventDef event;
        event.function = "onMontageEvent";
        montage.events.push_back(event);
        changed = true;
        undo = "Add Montage Event";
    }
    ImGui::SetItemTooltip("A function of the GameObject's scripts called at that time (also when the montage is blending)");
    if (changed) stageMontage(document, montage, undo);
}
