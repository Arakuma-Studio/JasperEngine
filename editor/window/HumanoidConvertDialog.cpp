// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HumanoidConvertDialog.h"

#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "animation/AnimationClipDocument.h"
#include "animation/ClipConverters.h"
#include "animation/ModelSubClips.h"
#include "command/type/ClipSceneActions.h"
#include "imgui.h"
#include "util/FileUtils.h"
#include "window/EditorModal.h"

#include <cstdio>
#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    struct DialogState{
        bool requested = false;
        std::string source;
        uint32_t sceneId = 0;
        Entity player = NULL_ENTITY;
        size_t entry = SIZE_MAX;
        char model[512] = "";
        char output[512] = "";
        bool usePlayer = true;
        std::string error;
    };

    DialogState& state(){
        static DialogState instance;
        return instance;
    }

}

void HumanoidConvertDialog::open(Project* project, const std::string& source, uint32_t sceneId, Entity player, size_t entry){
    DialogState& s = state();
    s = DialogState();
    s.requested = true;
    s.source = source;
    s.sceneId = sceneId;
    s.player = player;
    s.entry = entry;
    // the defaults: the clip's source model, "<name> (Humanoid).anim" next to it
    const std::string reference = ClipConverters::playerEntry(project, source).clip;
    ClipData data;
    if (!anim::isSubClipReference(reference)){
        std::string error;
        const fs::path file = project->resolveAssetPath(fs::u8path(reference));
        const std::string relative = FileUtils::pathToGenericUtf8(file.lexically_normal().lexically_relative(project->getProjectPath()));
        if (std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::open(project, relative, &error)){
            data = document->data();
        }
    }
    std::snprintf(s.model, sizeof(s.model), "%s", ClipConverters::humanoidSourceModel(reference, data).c_str());
    std::snprintf(s.output, sizeof(s.output), "%s", ClipSceneActions::humanoidClipPath(project, reference).c_str());
}

void HumanoidConvertDialog::draw(Project* project){
    DialogState& s = state();
    if (s.requested){
        s.requested = false;
        ImGui::OpenPopup("Convert to Humanoid Clip###HumanoidConvert");
    }
    if (!EditorModal::begin("Convert to Humanoid Clip###HumanoidConvert", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    EditorModal::message("Convert " + s.source + " to a humanoid clip.");
    EditorModal::error(s.error);
    ImGui::TextUnformatted(s.source.c_str());
    ImGui::TextColored(Theme::Colors::SubtleText, "A humanoid clip plays on any humanoid character (its rotations are relative to\n"
                       "the neutral pose of the rig it was made on).");
    ImGui::SetNextItemWidth(Theme::dpi(360.0f));
    EditorModal::pathField("model", s.model, sizeof(s.model));
    ImGui::InputText("Source model", s.model, sizeof(s.model));
    ImGui::SetItemTooltip("The model (asset-relative .gltf / .glb / .fbx) whose rig the clip was made on; its .humanoid Avatar applies");
    ImGui::SetNextItemWidth(Theme::dpi(360.0f));
    EditorModal::pathField("output", s.output, sizeof(s.output));
    ImGui::InputText("Output", s.output, sizeof(s.output));
    ImGui::SetItemTooltip("The new .anim (project-relative)");
    if (s.player != NULL_ENTITY){
        EditorModal::checkbox("Use in this player", "use_in_player", &s.usePlayer);
        ImGui::SetItemTooltip("The Animation Player entry plays the new clip (one scene undo step with the file)");
    }
    if (!s.error.empty()) ImGui::TextColored(Theme::Colors::ErrorText, "%s", s.error.c_str());
    ImGui::BeginDisabled(s.model[0] == '\0' || s.output[0] == '\0');
    if (EditorModal::button("Convert", "ok")){
        ClipActionResult result;
        std::string error;
        const bool use = s.player != NULL_ENTITY && s.usePlayer;
        if (ClipSceneActions::convertToHumanoid(project, s.sceneId, use ? s.player : NULL_ENTITY, s.entry, s.source, s.model, s.output, result, error)){
            Out::info("%s", result.status.c_str());
            for (const ClipWarning& warning : result.warnings) Out::warning("%s: %s", result.clip.c_str(), warning.message.c_str());
            ImGui::CloseCurrentPopup();
        }else{
            s.error = error;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (EditorModal::button("Cancel", "cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
