// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AvatarWindow.h"

#include "Project.h"
#include "Scene.h"
#include "Theme.h"
#include "animation/AnimationClip.h"
#include "animation/KeyframeSampling.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "external/IconsFontAwesome6.h"
#include "imgui.h"
#include "pool/AnimationClipPool.h"
#include "pose/ModelRig.h"
#include "pose/PoseDofs.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/ProjectFileWrites.h"
#include "util/Util.h"

#include <algorithm>
#include <cctype>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    std::string lower(std::string text){
        for (char& c : text) c = (char)std::tolower((unsigned char)c);
        return text;
    }

    // a combo of the skeleton's bones with Auto and None (true: `value` changed)
    bool boneCombo(const char* id, const std::optional<std::string>& value, const std::string& resolved,
                   const std::vector<std::string>& bones, const std::string& filter, std::optional<std::string>& out){
        const std::string preview = value ? (value->empty() ? std::string("None") : *value)
                                          : (resolved.empty() ? std::string("Auto (not found)") : "Auto: " + resolved);
        bool changed = false;
        if (ImGui::BeginCombo(id, preview.c_str(), ImGuiComboFlags_HeightLarge)){
            if (ImGui::Selectable("Auto", !value.has_value())){
                out.reset();
                changed = true;
            }
            ImGui::SetItemTooltip("Back to the name-based detection");
            if (ImGui::Selectable("None", value && value->empty())){
                out = std::string();
                changed = true;
            }
            ImGui::SetItemTooltip("Forced unmapped");
            ImGui::Separator();
            for (const std::string& bone : bones){
                if (!filter.empty() && lower(bone).find(filter) == std::string::npos) continue;
                if (ImGui::Selectable(bone.c_str(), value && *value == bone)){
                    out = bone;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    const char* rowState(bool overridden, bool mapped, bool required){
        if (overridden) return mapped ? "override" : "none";
        if (!mapped) return required ? "missing" : "not found";
        return "auto";
    }

}

editor::AvatarWindow::AvatarWindow(Project* project): project(project){
}

editor::AvatarWindow::~AvatarWindow(){
}

void editor::AvatarWindow::setOpen(bool open){
    if (!open) stopPreview();
    windowOpen = open;
}

Scene* editor::AvatarWindow::scene() const{
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene || !sceneProject->scene->isEntityCreated(entity)) return nullptr;
    return sceneProject->scene;
}

bool editor::AvatarWindow::openModel(uint32_t id, Entity target, std::string* error){
    stopPreview();
    SceneProject* sceneProject = project ? project->getScene(id) : nullptr;
    Scene* sceneptr = sceneProject ? sceneProject->scene : nullptr;
    const ModelComponent* component = sceneptr && sceneptr->isEntityCreated(target) ? sceneptr->findComponent<ModelComponent>(target) : nullptr;
    if (!component || component->filename.empty()){
        if (error) *error = "the GameObject has no Model";
        return false;
    }
    pose::Skeleton built;
    std::vector<Entity> entities;
    std::vector<Matrix4> folded;
    if (!pose::buildSkeleton(sceneptr, target, built, entities, &folded)){
        if (error) *error = "the model '" + component->filename + "' has no bones: an Avatar needs a skinned model";
        return false;
    }
    sceneId = id;
    entity = target;
    model = component->filename;
    skeleton = built;
    boneEntities = entities;
    bridges = folded;
    bones.clear();
    for (const pose::SkeletonBone& bone : skeleton.bones) bones.push_back(bone.name);
    sidecar = project->resolveAssetPath(FileUtils::pathFromUtf8(pose::humanoidSidecarPath(model)));
    saved = pose::HumanoidOverrides();
    std::error_code ec;
    status.clear();
    statusError = false;
    if (fs::is_regular_file(sidecar, ec)){
        std::string text;
        std::vector<std::string> warnings;
        std::string why;
        if (!ProjectChecks::readFile(sidecar, text) || !pose::parseHumanoidOverrides(text, saved, warnings, &why)){
            status = "the sidecar cannot be read (" + why + "): Apply writes a new one";
            statusError = true;
            saved = pose::HumanoidOverrides();
        }
    }
    edited = saved;
    testLoaded.reset();
    resolve();
    return true;
}

void editor::AvatarWindow::resolve(){
    current = Resolved();
    std::string error;
    if (!pose::resolveHumanoid(skeleton, edited, current.humanoid, current.extra, &error, &current.notes)){
        current.error = error;
        // what auto-detect and the overrides give, for the rows, even when it does not validate
        pose::HumanoidMap map;
        std::string ignored;
        pose::resolveHumanoid(skeleton, edited, map, &ignored, nullptr);
        current.humanoid = map;
        current.extra = pose::autoDetectExtraBones(skeleton, map);
        for (int x = 0; x < pose::HUMAN_EXTRA_BONE_COUNT; x++){
            if (edited.extraBones[(size_t)x]) current.extra.bones[(size_t)x] = *edited.extraBones[(size_t)x];
        }
        return;
    }
    Scene* sceneptr = scene();
    auto rig = std::make_shared<pose::PoseRig>();
    if (sceneptr) rig->setSkin(pose::buildSkin(sceneptr, entity, skeleton, boneEntities));
    const std::array<float, pose::HUMAN_BONE_COUNT> twist = edited.twistDegrees();
    if (!rig->build(skeleton, current.humanoid, &error, &twist)){
        current.error = error;
        return;
    }
    current.rig = rig;
}

void editor::AvatarWindow::setBone(pose::HumanBone slot, const std::optional<std::string>& bone){
    edited.bones[(size_t)pose::boneIndex(slot)] = bone;
    resolve();
}

void editor::AvatarWindow::setExtraBone(pose::HumanExtraBone bone, const std::optional<std::string>& name){
    edited.extraBones[(size_t)pose::extraIndex(bone)] = name;
    resolve();
}

void editor::AvatarWindow::setTwist(pose::HumanBone slot, float degrees){
    const float clamped = std::isfinite(degrees) ? std::clamp(degrees, -pose::HUMANOID_MAX_TWIST, pose::HUMANOID_MAX_TWIST) : 0.0f;
    if (clamped == 0.0f) edited.twist[(size_t)pose::boneIndex(slot)].reset();
    else edited.twist[(size_t)pose::boneIndex(slot)] = clamped;
    resolve();
}

void editor::AvatarWindow::setAnimationType(pose::AnimationType type){
    // generic is the default: not written (a bones-only sidecar keeps its version 1 bytes)
    if (type == pose::AnimationType::Humanoid) edited.animationType = type;
    else edited.animationType.reset();
}

void editor::AvatarWindow::autoMapAll(){
    for (auto& bone : edited.bones) bone.reset();
    for (auto& bone : edited.extraBones) bone.reset();
    resolve();
}

void editor::AvatarWindow::clearOverrides(){
    const std::optional<pose::AnimationType> type = edited.animationType;
    edited = pose::HumanoidOverrides();
    edited.animationType = type;
    resolve();
}

bool editor::AvatarWindow::apply(std::string* error){
    if (sidecar.empty()){
        if (error) *error = "no model";
        return false;
    }
    std::string why;
    if (!ProjectFileWrites::writeUndoable(project, {{sidecar, pose::writeHumanoidOverrides(edited)}}, why)){
        if (error) *error = why;
        status = why;
        statusError = true;
        return false;
    }
    saved = edited;
    status = "Applied: " + fs::u8path(model).filename().u8string() + ".humanoid written (one undo step)";
    statusError = false;
    return true;
}

void editor::AvatarWindow::discard(){
    edited = saved;
    resolve();
}

// ---- preview ----

std::shared_ptr<anim::HumanoidTarget> editor::AvatarWindow::previewTarget() const{
    if (!current.rig) return nullptr;
    pose::ModelRigInfo info;
    info.rig = current.rig;
    info.humanoid = current.humanoid;
    info.extra = current.extra;
    auto target = std::make_shared<anim::HumanoidTarget>();
    if (!anim::buildHumanoidTarget(info, boneEntities, bridges, entity, model, *target)) return nullptr;
    return target;
}

void editor::AvatarWindow::setPreview(bool on){
    if (on == previewing) return;
    if (!on){
        stopPreview();
        return;
    }
    Scene* sceneptr = scene();
    if (!sceneptr) return;
    savedTransforms.clear();
    for (Entity bone : boneEntities){
        if (const Transform* transform = sceneptr->findComponent<Transform>(bone)){
            savedTransforms.push_back({bone, transform->position, transform->rotation});
        }
    }
    previewing = true;
    updatePreview();
}

void editor::AvatarWindow::setTestClip(const std::string& clip){
    testClip = clip;
    testLoaded.reset();
    testError.clear();
    testTime = 0.0f;
    std::snprintf(testClipText, sizeof(testClipText), "%s", clip.c_str());
    if (clip.empty()) return;
    // played retargeted: a generic clip converts from its source model (anim-retarget 7.4)
    testLoaded = AnimationClipPool::getOrLoad(clip + "@humanoid", &testError);
}

void editor::AvatarWindow::setTestTime(float seconds){
    testTime = std::isfinite(seconds) ? std::max(0.0f, seconds) : 0.0f;
}

void editor::AvatarWindow::updatePreview(){
    if (!previewing) return;
    Scene* sceneptr = scene();
    std::shared_ptr<anim::HumanoidTarget> target = previewTarget();
    if (!sceneptr || !target) return;
    pose::HumanPose pose = current.rig->neutralPose();
    std::array<Quaternion, pose::HUMAN_EXTRA_BONE_COUNT> extra;
    std::vector<anim::HumanoidOutput> outputs;
    if (testLoaded){
        anim::HumanoidBinding binding;
        if (anim::bindHumanoidClip(*testLoaded, target, binding)){
            binding.mirror = mirror;
            binding.footIK = footIK;
            anim::sampleHumanoidBinding(*testLoaded, binding, anim::wrapClipTime(testTime, testLoaded->getLength(), ClipWrapMode::Loop), nullptr);
            pose = binding.pose;
            outputs = binding.outputs;
        }
    }
    // the neutral pose of every slot; the clip's bones on top
    for (int b = 0; b < pose::HUMAN_BONE_COUNT; b++){
        const Entity bone = target->slotEntities[(size_t)b];
        Transform* transform = bone != NULL_ENTITY ? sceneptr->findComponent<Transform>(bone) : nullptr;
        if (!transform) continue;
        transform->rotation = target->bridgeRotationInverse[(size_t)b] * pose.rotations[(size_t)b];
        if (b == 0) transform->position = target->hipsBridgeInverse * pose.hipsPosition;
        transform->needUpdate = true;
    }
    for (const anim::HumanoidOutput& output : outputs){
        if (output.kind != anim::HumanoidOutput::Kind::Extra) continue;
        if (Transform* transform = sceneptr->findComponent<Transform>(output.entity)){
            transform->rotation = output.rotation;
            transform->needUpdate = true;
        }
    }
    (void)extra;
}

void editor::AvatarWindow::stopPreview(){
    if (!previewing) return;
    previewing = false;
    testPlaying = false;
    Scene* sceneptr = scene();
    if (!sceneptr) return;
    for (const Saved& entry : savedTransforms){
        if (Transform* transform = sceneptr->isEntityCreated(entry.entity) ? sceneptr->findComponent<Transform>(entry.entity) : nullptr){
            transform->position = entry.position;
            transform->rotation = entry.rotation;
            transform->needUpdate = true;
        }
    }
    savedTransforms.clear();
}

void editor::AvatarWindow::stopTransientPreviews(){
    stopPreview();
}

void editor::AvatarWindow::clearSceneState(uint32_t id){
    if (id != sceneId) return;
    previewing = false;
    savedTransforms.clear();
    entity = NULL_ENTITY;
    model.clear();
    sidecar.clear();
}

void editor::AvatarWindow::projectClosed(){
    previewing = false;
    savedTransforms.clear();
    entity = NULL_ENTITY;
    model.clear();
    sidecar.clear();
    windowOpen = false;
}

// ---- drawing ----

void editor::AvatarWindow::show(){
    if (!windowOpen) return;
    if (focusRequested){
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(620.0f), Theme::dpi(640.0f)), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin(WINDOW_NAME, &open)){
        if (!scene() || model.empty()){
            ImGui::TextWrapped("Select a skinned model and use its Inspector's Configure Avatar... (Model > Rig).");
            if (previewing) stopPreview();
        }else{
            drawHeader();
            if (ImGui::BeginTabBar("##AvatarTabs")){
                if (ImGui::BeginTabItem("Mapping")){
                    if (tab != 0 && previewing) stopPreview();
                    tab = 0;
                    drawMapping();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Pose")){
                    tab = 1;
                    drawPose();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Notes")){
                    if (tab != 2 && previewing) stopPreview();
                    tab = 2;
                    drawNotes();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        drawClosePrompt();
    }
    ImGui::End();
    if (!open){
        // closing with changes asks first
        if (isDirty()) closePrompt = true;
        else setOpen(false);
    }
    if (previewing) updatePreview();
}

void editor::AvatarWindow::drawHeader(){
    ImGui::TextUnformatted(fs::u8path(model).filename().u8string().c_str());
    ImGui::SameLine();
    const bool humanoid = edited.isHumanoid();
    ImGui::SetNextItemWidth(Theme::dpi(130.0f));
    if (ImGui::BeginCombo("Animation Type", humanoid ? "Humanoid" : "Generic")){
        if (ImGui::Selectable("Generic", !humanoid)) setAnimationType(pose::AnimationType::Generic);
        ImGui::SetItemTooltip("Its clips play as they are (absolute bone rotations), on characters of this rig");
        if (ImGui::Selectable("Humanoid", humanoid)) setAnimationType(pose::AnimationType::Humanoid);
        ImGui::SetItemTooltip("Its clips play retargeted on any humanoid character");
        ImGui::EndCombo();
    }
    // the status chip
    const int slots = 22 - (int)std::count_if(current.humanoid.bones.begin(), current.humanoid.bones.end(), [](const std::string& b){ return b.empty(); });
    if (current.rig){
        const int fingers = current.extra.mappedCount(0, pose::HUMAN_FINGER_BONE_COUNT);
        const int eyes = current.extra.mappedCount(pose::extraIndex(pose::HumanExtraBone::LeftEye), 2);
        const bool jaw = !current.extra.bones[(size_t)pose::extraIndex(pose::HumanExtraBone::Jaw)].empty();
        ImGui::TextColored(Theme::Colors::SubtleText, ICON_FA_BONE " Humanoid %d/22 · fingers %d/30 · eyes %d%s · human scale %.3f",
                           slots, fingers, eyes, jaw ? " · jaw" : "", current.rig->humanScale());
    }else{
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " Not a humanoid: %s", current.error.c_str());
    }
    ImGui::BeginDisabled(!isDirty());
    if (ImGui::Button("Apply")){
        std::string error;
        apply(&error);
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard")) discard();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Back to the sidecar as saved");
    if (!status.empty()){
        ImGui::SameLine();
        ImGui::TextColored(statusError ? Theme::Colors::ErrorText : Theme::Colors::SubtleText, "%s", status.c_str());
    }
    ImGui::Separator();
}

void editor::AvatarWindow::drawMapping(){
    if (ImGui::Button("Auto Map All")) autoMapAll();
    ImGui::SetItemTooltip("Every body and hand bone back to the name-based detection");
    ImGui::SameLine();
    if (ImGui::Button("Clear Overrides")) clearOverrides();
    ImGui::SetItemTooltip("The mapping back to auto-detect and no twist corrections (the Animation Type stays)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(180.0f));
    ImGui::InputTextWithHint("##AvatarFilter", ICON_FA_MAGNIFYING_GLASS " filter bones", boneFilter, sizeof(boneFilter));
    const std::string filter = lower(boneFilter);

    auto row = [&](const char* name, bool required, const std::optional<std::string>& value, const std::string& resolved,
                   std::optional<std::string>& out){
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        const bool mapped = !resolved.empty() && !(value && value->empty());
        const bool missing = required && !mapped;
        const ImVec4 colour = missing ? Theme::Colors::ErrorText : value ? ImGui::GetStyle().Colors[ImGuiCol_Text] : Theme::Colors::SubtleText;
        ImGui::TextColored(colour, "%s%s", name, required ? " *" : "");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1.0f);
        const bool changed = boneCombo("##bone", value, resolved, bones, filter, out);
        ImGui::TableSetColumnIndex(2);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(colour, "%s", rowState(value.has_value(), mapped, required));
        return changed;
    };
    const float height = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginTable("##AvatarMapping", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH, ImVec2(0, height))){
        ImGui::TableSetupColumn("Bone", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(170.0f));
        ImGui::TableSetupColumn("Skeleton bone", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(70.0f));
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(Theme::Colors::SubtleText, "Body");
        for (int b = 0; b < pose::HUMAN_BONE_COUNT; b++){
            const pose::HumanBone slot = pose::boneAt(b);
            ImGui::PushID(b);
            std::optional<std::string> out;
            if (row(pose::humanBoneName(slot), pose::humanBoneInfo(slot).required, edited.bones[(size_t)b], current.humanoid.bones[(size_t)b], out)){
                setBone(slot, out);
            }
            ImGui::PopID();
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(Theme::Colors::SubtleText, "Hands & Head");
        for (int x = 0; x < pose::HUMAN_EXTRA_BONE_COUNT; x++){
            const pose::HumanExtraBone bone = pose::extraAt(x);
            ImGui::PushID(100 + x);
            std::optional<std::string> out;
            if (row(pose::humanExtraBoneName(bone), false, edited.extraBones[(size_t)x], current.extra.bones[(size_t)x], out)){
                setExtraBone(bone, out);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void editor::AvatarWindow::drawPose(){
    bool preview = previewing;
    if (ImGui::Checkbox("Show the neutral pose in the Scene view", &preview)) setPreview(preview);
    ImGui::SetItemTooltip("The rig's computed neutral standing pose: every humanoid clip is relative to it");
    ImGui::TextColored(Theme::Colors::SubtleText, "Twist corrections turn a bone's neutral pose about its direction: they apply to every "
                       "humanoid clip and pose animation on this model.");
    // the test clip
    ImGui::SetNextItemWidth(Theme::dpi(300.0f));
    if (ImGui::InputTextWithHint("Test clip", "models/hero.gltf#Walk or anim/walk.anim", testClipText, sizeof(testClipText),
                                 ImGuiInputTextFlags_EnterReturnsTrue)){
        setTestClip(testClipText);
        if (!previewing) setPreview(true);
    }
    if (ImGui::BeginDragDropTarget()){
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
            const std::vector<std::string> files = Util::getStringsFromPayload(payload);
            if (!files.empty()){
                setTestClip(FileUtils::pathToGenericUtf8(project->normalizeToAssetsRelative(FileUtils::pathFromUtf8(files[0]))));
                if (!previewing) setPreview(true);
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (!testError.empty()) ImGui::TextColored(Theme::Colors::ErrorText, "%s", testError.c_str());
    if (testLoaded){
        if (ImGui::Button(testPlaying ? ICON_FA_PAUSE : ICON_FA_PLAY)) testPlaying = !testPlaying;
        ImGui::SameLine();
        float time = testTime;
        ImGui::SetNextItemWidth(Theme::dpi(220.0f));
        if (ImGui::SliderFloat("##TestTime", &time, 0.0f, std::max(0.01f, testLoaded->getLength()), "%.2f s")) setTestTime(time);
        if (testPlaying){
            testTime += ImGui::GetIO().DeltaTime;
            if (testTime > testLoaded->getLength()) testTime = 0.0f;
        }
        ImGui::SameLine();
        ImGui::Checkbox("Mirror", &mirror);
        ImGui::SameLine();
        ImGui::Checkbox("Foot IK", &footIK);
    }
    ImGui::Separator();
    if (!current.rig){
        ImGui::TextColored(Theme::Colors::WarningText, "Map the bones first (Mapping tab): %s", current.error.c_str());
        return;
    }
    if (ImGui::BeginTable("##AvatarTwist", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, ImGui::GetContentRegionAvail().y))){
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(170.0f));
        ImGui::TableSetupColumn("Twist", ImGuiTableColumnFlags_WidthStretch);
        for (int b = 0; b < pose::HUMAN_BONE_COUNT; b++){
            const pose::HumanBone slot = pose::boneAt(b);
            if (!current.rig->has(slot)) continue;
            ImGui::PushID(b);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(pose::humanBoneName(slot));
            ImGui::TableSetColumnIndex(1);
            float degrees = edited.twist[(size_t)b].value_or(0.0f);
            ImGui::SetNextItemWidth(-Theme::dpi(30.0f));
            if (ImGui::SliderFloat("##twist", &degrees, -180.0f, 180.0f, "%.1f deg")) setTwist(slot, degrees);
            ImGui::SameLine();
            ImGui::BeginDisabled(!edited.twist[(size_t)b].has_value());
            if (ImGui::SmallButton(ICON_FA_ROTATE_LEFT)) setTwist(slot, 0.0f);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("No correction");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void editor::AvatarWindow::drawNotes(){
    if (!current.error.empty()) ImGui::TextColored(Theme::Colors::ErrorText, "%s", current.error.c_str());
    if (current.notes.empty()) ImGui::TextColored(Theme::Colors::SubtleText, "Every bone was found by its name.");
    for (const std::string& note : current.notes) ImGui::BulletText("%s", note.c_str());
}

void editor::AvatarWindow::drawClosePrompt(){
    if (closePrompt){
        ImGui::OpenPopup("Unsaved Avatar###AvatarClose");
        closePrompt = false;
    }
    if (ImGui::BeginPopupModal("Unsaved Avatar###AvatarClose", nullptr, ImGuiWindowFlags_AlwaysAutoResize)){
        ImGui::Text("Apply the changes to %s.humanoid?", fs::u8path(model).filename().u8string().c_str());
        if (ImGui::Button("Apply")){
            std::string error;
            if (apply(&error)) setOpen(false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")){
            discard();
            setOpen(false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
