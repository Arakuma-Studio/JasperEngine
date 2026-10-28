// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation window's Record mode and saved settings (animation clips spec 4.4, 9.R5/R5b/R50).
// - Record (the red button after Preview, R): the controller's record listener claims edits of
//   the previewed hierarchy; the sources (SceneRender's gizmo drag, the Inspector's Transform and
//   Blend Shapes rows, LatticeEditorOps' drags) then write the live value and key it at the
//   playhead with one merge key per gesture, no scene command. Leaving Record stops the preview:
//   the scene goes back to its snapshot. Ctrl+Z goes to the project history while the newest step
//   is a recorded key (R5b, the controller's undo routing).
// - Settings: the label column width and the Dopesheet / Curves choice are kept in the editor's
//   layout ini (an ImGui settings handler, [JasperAnimationWindow][Clip], registered by the App
//   before the ini loads: registerSettingsHandler), applied on the window's first frame and
//   written when they change.

#include "AnimationWindowClip.h"
#include "AnimationWindowClipUtil.h"

#include "imgui_internal.h"
#include "external/IconsFontAwesome6.h"
#include "Project.h"
#include "animation/AnimationClipDocument.h"
#include "animation/ClipBinding.h"

#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // what the layout ini holds (one Animation window)
    struct SavedSettings{
        float labelWidth = -1.0f;
        int view = -1;
    };

    SavedSettings& saved(){
        static SavedSettings settings;
        return settings;
    }

    constexpr const char* SETTINGS_TYPE = "JasperAnimationWindow";

    void readLine(const char* line){
        float width = 0.0f;
        int view = 0;
        if (std::sscanf(line, "LabelWidth=%f", &width) == 1 && std::isfinite(width)) saved().labelWidth = width;
        else if (std::sscanf(line, "View=%d", &view) == 1) saved().view = view;
    }

}

// ---- Record ----

bool AnimationWindowClip::setRecording(SceneProject* sceneProject, bool on, std::string* error){
    AnimationClipController& c = controller();
    if (on == c.isRecording()) return true;
    if (!on){
        // leaving Record goes back to how it was before (Unity): the preview stops (the scene
        // goes back to its snapshot) unless it was on before Record
        c.setRecording(false);
        if (!previewBeforeRecord) stopPreview();
        return true;
    }
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc){
        if (error) *error = "Record: open a clip first";
        return false;
    }
    if (doc->isReadOnly()){
        if (error) *error = "Record: '" + doc->path() + "' is a model's animation (read-only): Extract to .anim to edit it";
        return false;
    }
    if (sceneProject && sceneProject->playState != ScenePlayState::STOPPED){
        if (error) *error = "Record: not while the scene plays";
        return false;
    }
    // Record keys what the preview shows (a target-less clip previews on its model)
    previewBeforeRecord = c.isPreviewing();
    if (!c.isPreviewing() && !startPreview(sceneProject, error)) return false;
    c.setRecording(true);
    if (!c.isRecording()){
        if (error) *error = "Record could not start";
        return false;
    }
    return true;
}

bool AnimationWindowClip::isRecording() const{
    return controllerUsed && controller().isRecording();
}

std::string AnimationWindowClip::recordStatus(bool full) const{
    if (!controllerUsed) return std::string();
    AnimationClipController& c = controller();
    if (!c.isRecording()) return std::string();
    SceneProject* sceneProject = c.getPreviewSceneId() ? project->getScene(c.getPreviewSceneId()) : nullptr;
    const Entity root = c.getPreviewRoot();
    if (!sceneProject || !sceneProject->scene || root == NULL_ENTITY || !sceneProject->scene->isEntityCreated(root)) return std::string();
    Scene* scene = sceneProject->scene;
    const std::string rootName = scene->getEntityName(root);
    const std::string frame = std::to_string((long)std::lround(c.getTime() * frameRate()));
    // selected GameObjects Record does not key stay as they are when edited with the bound ones
    size_t outside = 0;
    std::string path;
    for (Entity entity : project->getSelectedEntities(sceneProject->id)){
        if (!scene->isEntityCreated(entity) || !anim::relativePath(scene, root, entity, path)) outside++;
    }
    if (!full){
        // the bottom status (the full text is its tooltip)
        std::string text = ICON_FA_CIRCLE " Rec frame " + frame + " \xC2\xB7 " + rootName;
        if (outside > 0) text += " \xC2\xB7 " + std::to_string(outside) + " not keyed";
        return text;
    }
    std::string text = "Recording: edits of " + rootName + " key frame " + frame;
    if (outside > 0){
        text += "; " + std::to_string(outside) + (outside == 1 ? " selected GameObject is" : " selected GameObjects are") +
                " not under " + rootName + " and stay" + (outside == 1 ? "s" : "") + " as " + (outside == 1 ? "it is" : "they are");
    }
    return text;
}

// ---- settings ----

void AnimationWindowClip::registerSettingsHandler(){
    // the App calls this right after creating the ImGui context, before the first frame loads the
    // ini: ImGui drops sections of unknown handlers at load and writes only registered ones, so a
    // session that never shows the window keeps the saved values (T3c review I6)
    if (!ImGui::GetCurrentContext() || ImGui::FindSettingsHandler(SETTINGS_TYPE)) return;
    ImGuiSettingsHandler handler;
    handler.TypeName = SETTINGS_TYPE;
    handler.TypeHash = ImHashStr(SETTINGS_TYPE);
    handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char*) -> void* {
        return reinterpret_cast<void*>(1);
    };
    handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line){ readLine(line); };
    handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf){
        const SavedSettings& values = saved();
        if (values.labelWidth < 0.0f && values.view < 0) return;
        buf->appendf("[%s][Clip]\n", handler->TypeName);
        if (values.labelWidth >= 0.0f) buf->appendf("LabelWidth=%.1f\n", values.labelWidth);
        if (values.view >= 0) buf->appendf("View=%d\n", values.view);
        buf->append("\n");
    };
    ImGui::AddSettingsHandler(&handler);
}

void AnimationWindowClip::loadSettings(){
    settingsLoaded = true;
    registerSettingsHandler();
    const SavedSettings& values = saved();
    if (values.labelWidth > 0.0f) labelWidth = std::clamp(values.labelWidth, 160.0f, 900.0f);
    if (values.view == 0) view = View::Dopesheet;
    if (values.view == 1) view = View::Curves;
}

void AnimationWindowClip::saveSettings(){
    registerSettingsHandler();
    SavedSettings& values = saved();
    const int shown = view == View::Curves ? 1 : 0;
    if (values.labelWidth == labelWidth && values.view == shown) return;
    values.labelWidth = labelWidth;
    values.view = shown;
    if (ImGui::GetCurrentContext()) ImGui::MarkIniSettingsDirty();
}
