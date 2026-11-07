// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PoseAnimationWindow.h"
#include "App.h"

#include "Backend.h"
#include "Scene.h"
#include "Theme.h"
#include "command/CommandHistory.h"
#include "command/type/WriteFilesCmd.h"
#include "component/BoneComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "external/IconsFontAwesome6.h"
#include "imgui_internal.h"
#include "pose/HumanoidMapIO.h"
#include "pose/PoseAnimationDocument.h"
#include "pose/PoseBaker.h"
#include "pose/PoseDofs.h"
#include "pose/PoseRanges.h"
#include "pose/PoseRigProvider.h"
#include "pose/PoseSampler.h"
#include "pose/PoseTween.h"
#include "util/EntityPayload.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/Util.h"

#include "pose/PoseInspectorHooks.h"
#include "animation/ClipRecordHooks.h"
#include "AppSettings.h"
#include "shortcuts/CommandCatalog.h"
#include "window/EditorModal.h"

#include <algorithm>
#include <map>
#include <cmath>
#include <cstdio>
#include <random>

using namespace jasperengine;

namespace fs = std::filesystem;

namespace {

    // card and chip geometry (spec 3.1: cards 100 px wide, thumbnails 88 x 116)
    constexpr float CARD_WIDTH = 100.0f;
    constexpr float THUMB_WIDTH = 88.0f;
    constexpr float THUMB_HEIGHT = 116.0f;
    constexpr float CHIP_WIDTH = 58.0f;
    constexpr float TIMING_HEIGHT = 50.0f;

    // fixed accents (the rest comes from the theme)
    const ImVec4 GLOBAL_EDIT_ORANGE(1.0f, 0.62f, 0.26f, 1.0f);     // #FF9F43, the phase 5 baseline ghost
    const ImVec4 PLAYHEAD_COLOR(1.0f, 0.42f, 0.33f, 1.0f);

    std::vector<editor::PoseAnimationWindow::Tab>& extraTabs(){
        static std::vector<editor::PoseAnimationWindow::Tab> tabs;
        return tabs;
    }

    ImU32 color(const ImVec4& c, float alpha = 1.0f){
        return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
    }

    std::string fixed(double value, int decimals){
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
        return buffer;
    }

    void caption(const char* text){
        ImGui::PushStyleColor(ImGuiCol_Text, editor::Theme::Colors::SubtleText);
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
    }

    // form fields stop growing past this width in a wide window
    constexpr float MAX_FIELD_WIDTH = 420.0f;

    float fieldWidth(){
        return std::min(ImGui::GetContentRegionAvail().x, editor::Theme::dpi(MAX_FIELD_WIDTH));
    }

    // a label column then the widget, the Inspector way
    void fieldLabel(const char* label, float labelWidth){
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(fieldWidth());
    }

    // the next item ends at the right edge of the line (after SameLine)
    void rightAlign(float width){
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - width));
    }

    bool keyChord(bool ctrl, bool shift, ImGuiKey key){
        const ImGuiIO& io = ImGui::GetIO();
        const bool command = io.KeyCtrl || io.KeySuper;
        return command == ctrl && io.KeyShift == shift && !io.KeyAlt && ImGui::IsKeyPressed(key, false);
    }

    // a context menu item's shortcut text: the catalog's binding (S25: rebindable)
    const char* shortcutText(const char* id){
        static std::map<std::string, std::string> texts;
        std::string& text = texts[id];
        text = editor::CommandCatalog::editor().menuShortcutText(id);
        return text.c_str();
    }

    std::string fileStem(const std::string& path){
        const fs::path p = editor::FileUtils::pathFromUtf8(path);
        return editor::FileUtils::pathToUtf8(p.stem());
    }

    const char* curveLabel(pose::TransitionCurve curve){
        switch (curve){
            case pose::TransitionCurve::Linear: return "Linear";
            case pose::TransitionCurve::Smooth: return "Smooth";
            case pose::TransitionCurve::Spring:
            default: return "Spring";
        }
    }

    // slider with a "--" display for mixed values; returns true when changed
    bool mixedSlider(const char* id, float& value, bool mixed, float min, float max, const char* format, bool drag = false, float speed = 0.01f){
        const char* shown = mixed ? "--" : format;
        if (drag){
            return ImGui::DragFloat(id, &value, speed, min, max, shown, ImGuiSliderFlags_AlwaysClamp);
        }
        return ImGui::SliderFloat(id, &value, min, max, shown, ImGuiSliderFlags_AlwaysClamp);
    }

}

// ---- construction ----

void editor::PoseAnimationWindow::addTab(Tab tab){
    auto& tabs = extraTabs();
    tabs.push_back(std::move(tab));
    std::stable_sort(tabs.begin(), tabs.end(), [](const Tab& a, const Tab& b){ return a.order < b.order; });
}

editor::PoseAnimationWindow::PoseAnimationWindow(Project* project): project(project), session(project), liveThumbnails(3){
    // the Pose Animator Inspector section's Open button
    poseInspectorHooks().open = [this](Project*, uint32_t sceneId, Entity model, const std::string& animation){
        openFromAsset(animation, sceneId, model);
    };
    // scene commands, undo and redo writing a posed bone's Transform edit its rest (one of the
    // chained channel write listeners, with the Animation window's clip preview: 9.R5)
    channelWriteListener = ClipRecordHooks::addBeforeChannelWrite(&PoseEditSession::beforeChannelWrite);
    // the Templates tab (phase 6, PoseAnimationWindowTemplates.cpp) through the tab extension point
    const auto& tabs = extraTabs();
    if (std::none_of(tabs.begin(), tabs.end(), [](const Tab& tab){ return tab.name == TEMPLATES_TAB; })){
        addTab({TEMPLATES_TAB, [](PoseAnimationWindow& window){ window.drawTemplatesTab(); }, 0});
    }
}

editor::PoseAnimationWindow::~PoseAnimationWindow(){
    poseInspectorHooks().open = nullptr;
    ClipRecordHooks::removeChannelWriteListener(channelWriteListener);
    if (PoseEditSession::active() == &session){
        PoseEditSession::setActive(nullptr);
    }
}

void editor::PoseAnimationWindow::setOpen(bool open){
    if (open){
        if (!windowOpen) focusRequested = true;
        windowOpen = true;
        PoseEditSession::setActive(&session);
        return;
    }
    windowOpen = false;
    windowFocused = false;
    windowVisible = false;
    focusRequested = false;
    session.setKeyRouting(false);
    // closing the window: Global Edit is cancelled and the bones go back
    session.cancelGlobalEdit();
    session.goIdle();
    if (PoseEditSession::active() == &session){
        PoseEditSession::setActive(nullptr);
    }
}

void editor::PoseAnimationWindow::requestFocus(){
    setOpen(true);
    focusRequested = true;
}

void editor::PoseAnimationWindow::selectTab(const std::string& name){
    // tab names are matched without case (MCP passes "feel")
    requestedTab = name;
    std::vector<std::string> names = {"Pose", "Feel", "Humanize"};
    for (const Tab& tab : extraTabs()) names.push_back(tab.name);
    for (const std::string& candidate : names){
        if (candidate.size() == name.size() && std::equal(candidate.begin(), candidate.end(), name.begin(),
                [](char a, char b){ return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); })){
            requestedTab = candidate;
        }
    }
}

bool editor::PoseAnimationWindow::openMappingFor(std::string* error){
    const uint32_t sceneId = session.bound() ? session.sceneId() : candidateScene;
    const Entity entity = session.bound() ? session.model() : candidateEntity;
    if (entity == NULL_ENTITY){
        if (error) *error = "no character: bind a model first";
        return false;
    }
    openMappingPopup(sceneId, entity);
    if (!mappingInfo){
        if (error) *error = "the character's model has no rig information";
        return false;
    }
    return true;
}

// ---- helpers ----

const pose::PoseAnimationData* editor::PoseAnimationWindow::data() const{
    const auto& doc = session.document();
    return doc && doc->exists() ? &doc->data() : nullptr;
}

std::shared_ptr<const pose::PoseAnimationData> editor::PoseAnimationWindow::view(){
    const auto& doc = session.document();
    if (!doc || !doc->exists()){
        viewData.reset();
        viewRevision = 0;
        return nullptr;
    }
    if (!viewData || viewDocument != doc.get() || viewRevision != doc->revision()){
        viewData = std::make_shared<const pose::PoseAnimationData>(doc->data());
        viewDocument = doc.get();
        viewRevision = doc->revision();
    }
    return viewData;
}

const std::vector<int>& editor::PoseAnimationWindow::selection() const{
    return session.selection();
}

std::string editor::PoseAnimationWindow::characterName() const{
    Scene* scene = nullptr;
    Entity entity = NULL_ENTITY;
    if (session.bound()){
        scene = session.scene();
        entity = session.model();
    }else if (candidateEntity != NULL_ENTITY){
        SceneProject* sceneProject = project->getScene(candidateScene);
        scene = sceneProject ? sceneProject->scene : nullptr;
        entity = candidateEntity;
    }
    if (!scene || entity == NULL_ENTITY || !scene->isEntityCreated(entity)) return "";
    std::string name = scene->getEntityName(entity);
    return name.empty() ? std::string("GameObject ") + std::to_string(entity) : name;
}

void editor::PoseAnimationWindow::setStatus(const std::string& text, bool error){
    statusLine = text;
    statusError = error;
    statusTime = ImGui::GetTime();
}

bool editor::PoseAnimationWindow::commit(const pose::PoseAnimationData& newData, const std::string& undoName, const std::string& mergeKey){
    std::string error;
    const pose::PoseAnimationData* current = data();
    // angle, offset, hold, transition, feel and humanize edits keep an active Global Edit
    const bool keep = poseui::keepsGlobalEdit(current ? current->poses.size() : 0, newData.poses.size(), false,
                                              session.primary(), session.primary());
    if (!session.commitData(newData, undoName, mergeKey, nullptr, -1, &error, keep)){
        setStatus(error, true);
        return false;
    }
    return true;
}

bool editor::PoseAnimationWindow::commitEdit(const poseui::Edit& edit, const std::string& undoName, bool reordered){
    if (!edit.changed){
        if (edit.primary >= 0 && edit.primary != session.primary()) session.setSelection(edit.selection, edit.primary);
        return false;
    }
    std::string error;
    const pose::PoseAnimationData* current = data();
    // mirror, paste, reset, ground and center keep it; add, delete and reorder cancel it
    const bool keep = poseui::keepsGlobalEdit(current ? current->poses.size() : 0, edit.data.poses.size(), reordered,
                                              session.primary(), edit.primary);
    if (!session.commitData(edit.data, undoName, "", &edit.selection, edit.primary, &error, keep)){
        setStatus(error, true);
        return false;
    }
    scrollToPrimary = true;
    return true;
}

std::string editor::PoseAnimationWindow::widgetMergeKey(const char* field){
    // one undo step per press of a widget: a new activation starts a new merge key
    if (ImGui::IsItemActivated() || !editActive){
        editSerial++;
        editActive = true;
    }
    return std::string(field) + "#" + std::to_string(editSerial);
}

void editor::PoseAnimationWindow::finishWidgetEdit(){
    if (ImGui::IsItemDeactivated()) editActive = false;
}

// ---- binding and documents ----

bool editor::PoseAnimationWindow::bindCharacter(uint32_t sceneId, Entity entity, std::string* error){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene){
        if (error) *error = "no such scene";
        return false;
    }
    if (sceneProject->playState != ScenePlayState::STOPPED){
        if (error) *error = "stop play mode to pose a character";
        return false;
    }
    candidateScene = sceneId;
    candidateEntity = entity;
    candidateError.clear();
    candidateInfo.reset();
    std::string failure;
    if (!session.bind(sceneProject->scene, sceneId, entity, &failure)){
        candidateError = failure;
        if (sceneProject->scene->isEntityCreated(entity) && sceneProject->scene->findComponent<ModelComponent>(entity)){
            candidateInfo = PoseRigProvider::forSceneModel(project, sceneProject->scene, entity);
        }
        if (error) *error = failure;
        return false;
    }
    if (bakeName.empty() && session.document()){
        bakeName = fileStem(session.document()->path());
    }
    return true;
}

bool editor::PoseAnimationWindow::openAnimation(const std::string& path, std::string* error){
    std::string failure;
    bool opened = session.open(path, &failure);
    if (!opened && !editor::FileUtils::pathFromUtf8(path).is_absolute()){
        // an assets-relative path (as components and preview_model store them)
        const fs::path inAssets = project->getAssetsPath() / editor::FileUtils::pathFromUtf8(path);
        std::string second;
        opened = session.open(editor::FileUtils::pathToUtf8(inAssets), &second);
    }
    if (!opened){
        if (error) *error = failure;
        return false;
    }
    bakeName = fileStem(session.document()->path());
    clipboard.reset();
    if (session.bound() && session.document() && !session.document()->data().poses.empty()){
        session.startPosing();
    }
    return true;
}

std::string editor::PoseAnimationWindow::createAsset(Project* project, const fs::path& directory, const std::string& baseName,
                                                     const std::string& previewModel, std::string* error){
    if (!project){
        if (error) *error = "no project";
        return "";
    }
    const std::string base = baseName.empty() ? std::string("New Pose Animation") : baseName;
    fs::path target;
    for (int n = 0; n < 1000; n++){
        const std::string name = n == 0 ? base : base + " " + std::to_string(n);
        target = directory / editor::FileUtils::pathFromUtf8(name + ".poseanim");
        std::error_code ec;
        if (!fs::exists(target, ec)) break;
    }
    pose::PoseAnimationData initial;   // the defaults are the Snappy preset
    initial.previewModel = previewModel;
    initial.poses.push_back(poseui::neutralPose({}));
    const std::string relative = ProjectChecks::projectRelative(project->getProjectPath(), target);
    std::string failure;
    std::shared_ptr<PoseAnimationDocument> created =
        PoseAnimationDocument::create(project, relative, initial, "Create pose animation", nullptr, &failure);
    if (!created){
        if (error) *error = failure;
        return "";
    }
    return created->path();
}

bool editor::PoseAnimationWindow::createAnimation(std::string* error){
    if (!session.bound()){
        if (error) *error = "pick a character first";
        return false;
    }
    ModelComponent* model = session.scene()->findComponent<ModelComponent>(session.model());
    if (!model){
        if (error) *error = "the character has no Model component";
        return false;
    }
    const fs::path modelFile = project->resolveAssetPath(model->filename);
    const std::string path = createAsset(project, modelFile.parent_path(), editor::FileUtils::pathToUtf8(editor::FileUtils::pathFromUtf8(model->filename.path).stem()),
                                         model->filename, error);
    if (path.empty()) return false;
    if (!openAnimation(path, error)) return false;
    setStatus("Created " + path, false);
    return true;
}

void editor::PoseAnimationWindow::openFromAsset(const std::string& path, uint32_t sceneId, Entity entity){
    requestFocus();
    pendingOpenPath = path;
    pendingOpenScene = sceneId;
    pendingOpenEntity = entity;
}

void editor::PoseAnimationWindow::processPendingOpen(){
    if (pendingOpenPath.empty() && pendingOpenEntity == NULL_ENTITY) return;
    const std::string path = pendingOpenPath;
    const uint32_t sceneId = pendingOpenScene;
    const Entity entity = pendingOpenEntity;
    pendingOpenPath.clear();
    pendingOpenEntity = NULL_ENTITY;
    std::string error;
    if (entity != NULL_ENTITY && !bindCharacter(sceneId, entity, &error)){
        setStatus(error, true);
    }
    if (!path.empty() && !openAnimation(path, &error)){
        setStatus(error, true);
        return;
    }
    // no character yet: the first model of the selected scene that uses the preview model
    const pose::PoseAnimationData* animation = data();
    if (!session.bound() && animation && !animation->previewModel.empty()){
        if (SceneProject* sceneProject = project->getSelectedScene()){
            if (sceneProject->scene && sceneProject->playState == ScenePlayState::STOPPED){
                for (Entity candidate : sceneProject->entities){
                    ModelComponent* model = sceneProject->scene->findComponent<ModelComponent>(candidate);
                    if (model && model->filename == animation->previewModel){
                        if (bindCharacter(sceneProject->id, candidate, nullptr)){
                            session.startPosing();
                        }
                        break;
                    }
                }
            }
        }
    }
}

void editor::PoseAnimationWindow::useSelection(){
    SceneProject* sceneProject = project->getSelectedScene();
    if (!sceneProject || !sceneProject->scene){
        setStatus("No scene is open", true);
        return;
    }
    const std::vector<Entity> picked = project->getSelectedEntities(sceneProject->id);
    for (Entity entity : picked){
        // a bone or a mesh of the model picks the model
        Entity candidate = entity;
        if (const BoneComponent* bone = sceneProject->scene->findComponent<BoneComponent>(entity)){
            if (bone->model != NULL_ENTITY) candidate = bone->model;
        }
        if (!sceneProject->scene->findComponent<ModelComponent>(candidate)){
            Transform* transform = sceneProject->scene->findComponent<Transform>(candidate);
            while (transform && transform->parent != NULL_ENTITY && !sceneProject->scene->findComponent<ModelComponent>(candidate)){
                candidate = transform->parent;
                transform = sceneProject->scene->findComponent<Transform>(candidate);
            }
        }
        if (sceneProject->scene->findComponent<ModelComponent>(candidate)){
            std::string error;
            if (bindCharacter(sceneProject->id, candidate, &error)){
                if (data() && !data()->poses.empty()) session.startPosing();
                setStatus("Character: " + characterName(), false);
            }else{
                setStatus(error, true);
            }
            return;
        }
    }
    setStatus("Select a GameObject with a Model in the Hierarchy or the Scene view", true);
}

void editor::PoseAnimationWindow::refreshCandidate(){
    // a bound character or candidate whose entity went away
    if (candidateEntity != NULL_ENTITY){
        SceneProject* sceneProject = project->getScene(candidateScene);
        if (!sceneProject || !sceneProject->scene || !sceneProject->scene->isEntityCreated(candidateEntity)){
            candidateEntity = NULL_ENTITY;
            candidateInfo.reset();
            candidateError.clear();
        }
    }
    if (session.bound()){
        candidateScene = session.sceneId();
        candidateEntity = session.model();
        candidateError.clear();
        candidateInfo.reset();
        wasBound = true;
    }else if (wasBound){
        // the session let go of a character that still exists (its mapping changed and it is no
        // usable humanoid any more): show why, with Map Bones
        wasBound = false;
        if (candidateEntity != NULL_ENTITY){
            bindCharacter(candidateScene, candidateEntity, nullptr);
        }
    }
}

void editor::PoseAnimationWindow::clearSceneState(uint32_t sceneId){
    session.clearSceneState(sceneId);
    if (candidateScene == sceneId){
        candidateEntity = NULL_ENTITY;
        candidateInfo.reset();
        candidateError.clear();
    }
    if (mappingScene == sceneId){
        mappingOpen = false;
        mappingInfo.reset();
    }
}

void editor::PoseAnimationWindow::stopTransientPreviews(){
    session.suspend();
}

void editor::PoseAnimationWindow::prepareForProjectSwitch(){
    session.unbind();
    session.closeDocument();
    thumbnails.clear();
    liveThumbnails.clear();
    candidateEntity = NULL_ENTITY;
    candidateInfo.reset();
    mappingOpen = false;
    mappingInfo.reset();
    clipboard.reset();
    statusLine.clear();
    // the next project's templates (its own user templates) are read again
    templateEntries.clear();
    templatesLoaded = false;
    templateStamp.clear();
    templateCheckTime = 0.0;
    templateRig.reset();
}

// ---- state (MCP) ----

editor::PoseAnimationWindow::State editor::PoseAnimationWindow::getState(){
    State state;
    state.open = windowOpen;
    state.character = characterName();
    state.sceneId = session.bound() ? session.sceneId() : candidateScene;
    state.entity = session.bound() ? session.model() : candidateEntity;
    if (session.bound() && session.rigInfo()){
        state.rig = poseui::rigStatus(session.rigInfo()->humanoid.bones).text;
    }else if (candidateInfo){
        state.rig = poseui::rigStatus(candidateInfo->humanoid.bones).text;
    }
    if (const auto& doc = session.document()){
        state.animation = doc->path();
        state.poseCount = static_cast<int>(doc->data().poses.size());
    }
    state.primary = session.primary();
    state.selection = session.selection();
    switch (session.mode()){
        case PoseEditSession::Mode::Posing: state.mode = "posing"; break;
        case PoseEditSession::Mode::Previewing: state.mode = "previewing"; break;
        default: state.mode = "idle"; break;
    }
    state.time = session.previewTime();
    state.length = session.previewLength();
    state.playing = session.previewPlaying();
    state.tab = activeTab;
    state.status = statusLine;
    state.emptyState = emptyState;
    state.sceneKeys = session.keyRouting();
    state.editable = session.editable();
    state.layout = compactLayout ? "columns" : "stacked";
    state.thumbnailHeight = thumbHeight;
    // the Templates tab (or an empty state's template grid) as drawn in the last frame
    if (windowOpen && templatesDrawnFrame >= ImGui::GetFrameCount() - 1){
        state.templates = templatesShown;
    }
    if (windowOpen && menuShownFrame >= ImGui::GetFrameCount() - 1){
        state.menu = menuShown;
    }
    return state;
}

// ---- transport ----

void editor::PoseAnimationWindow::selectPose(int index){
    actionSelect(index, false, false);
}

void editor::PoseAnimationWindow::play(){
    session.setPreviewPlaying(true);
}

void editor::PoseAnimationWindow::pause(){
    session.setPreviewPlaying(false);
}

void editor::PoseAnimationWindow::stop(){
    session.setPreviewPlaying(false);
    session.setPreviewTime(0.0f);
    session.stopPreview();
}

void editor::PoseAnimationWindow::bake(){
    const auto& doc = session.document();
    if (!doc || !session.bound()){
        setStatus("Pick a character and an animation to bake", true);
        return;
    }
    PoseBakeResult result;
    std::string error;
    const std::string name = bakeName.empty() ? fileStem(doc->path()) : bakeName;
    if (!PoseBaker::bake(project, session.sceneId(), session.model(), *doc, name, result, error)){
        setStatus("Bake failed: " + error, true);
        return;
    }
    setStatus("Baked " + std::to_string(result.frames) + " frames to '" + result.name + "' on " + characterName(), false);
}

// ---- actions ----

void editor::PoseAnimationWindow::actionSelect(int index, bool additive, bool range){
    if (session.mode() == PoseEditSession::Mode::Previewing){
        session.stopPreview();
    }
    session.select(index, additive, range);
    if (session.mode() == PoseEditSession::Mode::Idle && session.bound()){
        session.startPosing();
    }
    scrollToPrimary = true;
}

void editor::PoseAnimationWindow::actionAdd(){
    const pose::PoseAnimationData* animation = data();
    if (!animation) return;
    commitEdit(poseui::addPose(*animation, session.primary()), "Add pose");
    if (session.bound() && session.mode() == PoseEditSession::Mode::Idle) session.startPosing();
}

void editor::PoseAnimationWindow::actionDuplicate(bool atEnd, bool mirrored){
    const pose::PoseAnimationData* animation = data();
    if (!animation || selection().empty()) return;
    commitEdit(poseui::duplicatePoses(*animation, selection(), atEnd, mirrored), mirrored ? "Duplicate mirrored" : "Duplicate pose");
}

void editor::PoseAnimationWindow::actionMirror(){
    const pose::PoseAnimationData* animation = data();
    if (!animation) return;
    commitEdit(poseui::mirrorPoses(*animation, selection(), session.primary()), "Mirror pose");
}

void editor::PoseAnimationWindow::actionDelete(){
    const pose::PoseAnimationData* animation = data();
    if (!animation || selection().empty()) return;
    commitEdit(poseui::deletePoses(*animation, selection(), session.primary()), "Delete pose");
}

void editor::PoseAnimationWindow::actionCopy(){
    const pose::PoseAnimationData* animation = data();
    const int primary = session.primary();
    if (!animation || primary < 0 || primary >= static_cast<int>(animation->poses.size())) return;
    clipboard = animation->poses[primary];
    setStatus("Copied '" + clipboard->name + "'", false);
}

void editor::PoseAnimationWindow::actionPaste(){
    const pose::PoseAnimationData* animation = data();
    if (!animation || !clipboard) return;
    commitEdit(poseui::pastePose(*animation, selection(), session.primary(), *clipboard), "Paste pose");
}

void editor::PoseAnimationWindow::actionMove(int direction){
    const pose::PoseAnimationData* animation = data();
    if (!animation) return;
    const std::vector<int> order = poseui::reorderForShift(static_cast<int>(animation->poses.size()), selection(), direction);
    if (order.empty()) return;
    commitEdit(poseui::movePoses(*animation, selection(), session.primary(), order), "Move pose", true);
}

void editor::PoseAnimationWindow::actionMoveTo(int insertBefore){
    const pose::PoseAnimationData* animation = data();
    if (!animation) return;
    const std::vector<int> order = poseui::reorderForDrop(static_cast<int>(animation->poses.size()), selection(), insertBefore);
    if (order.empty()) return;
    commitEdit(poseui::movePoses(*animation, selection(), session.primary(), order), "Move pose", true);
}

void editor::PoseAnimationWindow::actionResetNeutral(){
    const pose::PoseAnimationData* animation = data();
    if (!animation) return;
    commitEdit(poseui::resetToNeutral(*animation, selection(), session.primary()), "Reset to neutral");
}

void editor::PoseAnimationWindow::actionGround(){
    const pose::PoseAnimationData* animation = data();
    if (!animation) return;
    commitEdit(poseui::groundPoses(*animation, selection(), session.primary()), "Ground pose");
}

void editor::PoseAnimationWindow::actionCenter(){
    const pose::PoseAnimationData* animation = data();
    if (!animation) return;
    commitEdit(poseui::centerPoses(*animation, selection(), session.primary()), "Center pose");
}

void editor::PoseAnimationWindow::actionRename(int index){
    const pose::PoseAnimationData* animation = data();
    if (!animation || index < 0 || index >= static_cast<int>(animation->poses.size())) return;
    renameIndex = index;
    renameFocus = true;
    std::snprintf(renameBuffer, sizeof(renameBuffer), "%s", animation->poses[index].name.c_str());
    // drawRenamePopup opens it at the window level (this may run inside the strip's child window)
}

void editor::PoseAnimationWindow::actionAutoTiming(){
    const pose::PoseAnimationData* animation = data();
    if (!animation || animation->poses.size() < 2 || !session.rig()){
        setStatus("Auto Timing needs a character and two poses", true);
        return;
    }
    std::shared_ptr<const PoseRealisation> realised = session.realisation();
    if (!realised) return;
    pose::PoseAnimationData timed = *animation;
    pose::autoTiming(timed, realised->keys, *session.rig());
    commit(timed, "Auto timing");
}

void editor::PoseAnimationWindow::actionResetCustom(){
    const pose::PoseAnimationData* animation = data();
    if (!animation) return;
    commitEdit(poseui::resetCustomTransitions(*animation, selection(), session.primary()), "Reset custom transitions");
}

void editor::PoseAnimationWindow::actionStepFrame(int direction){
    const pose::PoseAnimationData* animation = data();
    if (!animation || !session.bound()) return;
    if (session.mode() != PoseEditSession::Mode::Previewing){
        session.startPreview();
    }
    session.setPreviewPlaying(false);
    const float step = 1.0f / static_cast<float>(std::max(1, animation->settings.frameRate));
    const float length = session.previewLength();
    float t = session.previewTime() + direction * step;
    if (animation->settings.loop && length > 0.0f){
        t = std::fmod(t + length, length);
    }
    session.setPreviewTime(std::clamp(t, 0.0f, length));
}

void editor::PoseAnimationWindow::actionGlobalEdit(){
    if (session.globalEditActive()){
        session.cancelGlobalEdit();
        return;
    }
    if (!session.bound() || !data() || data()->poses.size() < 2){
        setStatus("Global Edit needs a character and two poses", true);
        return;
    }
    session.beginGlobalEdit();
}

// ---- shortcuts ----

bool editor::PoseAnimationWindow::ownsUndo(bool sceneViewFocused) const{
    if (!windowOpen) return false;
    // the window's own focus counts while it shows; a posing Scene view's keys also behind a tab
    if (windowFocused && windowVisible) return true;
    return sceneViewFocused && sceneKeysRouted();
}

bool editor::PoseAnimationWindow::sceneKeysRouted() const{
    // the Scene view poses while the user works on the character: it is selected (or one of its
    // bones), or a phase 5 handle is in use. Also with the window behind another tab of its dock:
    // the handles are live then, and the keys agree with them (final review ruling)
    if (!windowOpen || !session.bound() || session.mode() != PoseEditSession::Mode::Posing) return false;
    if (project->getSelectedSceneId() != session.sceneId()) return false;
    if (session.viewportHandleEngaged()) return true;
    const std::vector<Entity> picked = project->getSelectedEntities(session.sceneId());
    if (picked.empty()) return false;
    for (Entity entity : picked){
        if (!session.ownsEntity(session.sceneId(), entity)) return false;
    }
    return true;
}

void editor::PoseAnimationWindow::drawSceneKeysHint(){
    // a small "Pose keys" label over the Scene view while its keys and undo go to this window:
    // a tiny no-input overlay window (drawn with the windows, under popups and tooltips)
    if (!session.keyRouting()) return;
    const std::string name = "###Scene" + std::to_string(session.sceneId());
    ImGuiWindow* scene = ImGui::FindWindowByName(name.c_str());
    if (!scene || !scene->WasActive || scene->Hidden) return;
    const char* text = ICON_FA_PERSON " Pose keys (Pose Animation window)";
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 pad(Theme::dpi(6.0f), Theme::dpi(3.0f));
    ImGui::SetNextWindowPos(ImVec2(scene->InnerRect.Min.x + Theme::dpi(8.0f), scene->InnerRect.Max.y - size.y - pad.y * 2 - Theme::dpi(8.0f)));
    ImGui::SetNextWindowBgAlpha(0.78f);
    ImGui::SetNextWindowViewport(scene->ViewportId);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::dpi(4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##PoseKeysHint", nullptr, flags)){
        ImGui::TextColored(Theme::Colors::PrefabText, "%s", text);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

void editor::PoseAnimationWindow::handleLocalKeys(){
    // Esc stays local (S25: dialogs and gestures keep it); the window's other keys are the
    // command catalog's pose.* commands (bindCommands)
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || !data()) return;
    if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;
    if (keyChord(false, false, ImGuiKey_Escape)){
        if (session.globalEditActive()) session.cancelGlobalEdit();
        else if (session.mode() == PoseEditSession::Mode::Previewing) stop();
    }
}

void editor::PoseAnimationWindow::bindCommands(CommandCatalog& catalog, std::function<bool()> keysHere, std::function<bool()> fromSceneView){
    auto bind = [&](const char* id, std::function<void()> run, std::function<bool()> available = nullptr, bool notFromSceneView = false){
        const CommandDef* existing = catalog.find(id);
        if (!existing) return;
        CommandDef def = *existing;
        def.run = [run](const CommandArgs&){ run(); };
        def.enabled = [this, available](){ return data() != nullptr && (!available || available()); };
        def.keyGate = [keysHere, fromSceneView, notFromSceneView](){
            return !ImGui::GetIO().WantTextInput && (!keysHere || keysHere()) && !(notFromSceneView && fromSceneView && fromSceneView());
        };
        catalog.add(std::move(def));
    };
    auto count = [this](){ return data() ? static_cast<int>(data()->poses.size()) : 0; };
    auto any = [count](){ return count() > 0; };
    bind("pose.playPause", [this](){ if (session.previewPlaying()) pause(); else play(); });
    bind("pose.selectPrevious", [this](){ actionSelect(std::max(0, session.primary() - 1), false, false); }, any);
    bind("pose.selectNext", [this, count](){ actionSelect(std::min(count() - 1, session.primary() + 1), false, false); }, any);
    bind("pose.moveLeft", [this](){ actionMove(-1); }, any);
    bind("pose.moveRight", [this](){ actionMove(1); }, any);
    // the Scene view frames everything with Home
    bind("pose.selectFirst", [this](){ actionSelect(0, false, false); }, any, true);
    bind("pose.selectLast", [this, count](){ actionSelect(count() - 1, false, false); }, any, true);
    bind("pose.previousFrame", [this](){ actionStepFrame(-1); });
    bind("pose.nextFrame", [this](){ actionStepFrame(1); });
    // the rename popup opens with the window: not from a Scene view while it is behind a tab
    bind("pose.rename", [this](){ actionRename(session.primary()); }, [this](){ return windowVisible; });
    bind("pose.duplicate", [this](){ actionDuplicate(false, false); });
    bind("pose.duplicateMirrored", [this](){ actionDuplicate(false, true); });
    bind("pose.mirror", [this](){ actionMirror(); });
    bind("pose.delete", [this](){ actionDelete(); });
    bind("pose.ground", [this](){ actionGround(); });
    bind("pose.copy", [this](){ actionCopy(); });
    bind("pose.paste", [this](){ actionPaste(); });
    bind("pose.add", [this](){ actionAdd(); });
    // E: from a posing Scene view too (the catalog's pose context comes before the rotate tool's E)
    bind("pose.globalEdit", [this](){ actionGlobalEdit(); });
}

bool editor::PoseAnimationWindow::sceneKeysActive() const{
    return session.keyRouting();
}

// ---- frame ----

void editor::PoseAnimationWindow::updateThumbnails(){
    thumbnails.beginFrame();
    liveThumbnails.beginFrame();
    if (session.bound() && session.rigInfo()){
        const std::shared_ptr<const PoseRigInfo>& info = session.rigInfo();
        // absolute: PoseRender resolves relative paths against the assets folder
        const std::string modelFile = (project->getProjectPath() / editor::FileUtils::pathFromUtf8(info->model)).string();
        thumbnails.setModel(modelFile, session.rig(), info->bridges);
        liveThumbnails.setModel(modelFile, session.rig(), info->bridges);
    }else{
        if (!thumbnails.getModel().empty()) thumbnails.clear();
        if (!liveThumbnails.getModel().empty()) liveThumbnails.clear();
    }
}

void editor::PoseAnimationWindow::show(){
    session.update(ImGui::GetIO().DeltaTime);
    refreshCandidate();
    session.setKeyRouting(sceneKeysRouted());
    if (!windowOpen){
        windowFocused = false;
        windowVisible = false;
        return;
    }
    // the first open in a dock shorter than ~45% of the editor grows it, once ever
    if (growNode != 0){
        if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(growNode)){
            ImGui::DockBuilderSetNodeSize(growNode, ImVec2(node->Size.x, growHeight));
        }
        growNode = 0;
    }
    if (PoseEditSession::active() != &session){
        PoseEditSession::setActive(&session);
    }
    updateThumbnails();

    if (focusRequested){
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(Theme::dpi(320.0f), Theme::dpi(200.0f)), ImVec2(FLT_MAX, FLT_MAX));
    // a floating (undocked) window starts big enough for the strip, the timing bar and a tab
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(940.0f), Theme::dpi(640.0f)), ImGuiCond_FirstUseEver);
    bool open = windowOpen;
    windowVisible = ImGui::Begin(WINDOW_NAME, &open);
    windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (windowVisible && !AppSettings::getPoseDockGrown()){
        if (ImGuiDockNode* node = ImGui::GetWindowDockNode()){
            const float target = ImGui::GetMainViewport()->WorkSize.y * 0.45f;
            if (node->Size.y + 1.0f < target){
                growNode = node->ID;
                growHeight = target;
            }
            AppSettings::setPoseDockGrown(true);
            AppSettings::saveSettings();
        }
    }
    if (windowVisible){
        processPendingOpen();
        drawSetupRow();
        ImGui::Separator();
        if (!drawEmptyState()){
            // while the scene is saved or played the bones are not ours: nothing edits
            const bool locked = session.bound() && !session.editable();
            if (locked){
                ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s",
                                   project->isAnyScenePlaying() ? "Play mode: the pose animation is read-only"
                                                                : "Saving the scene: the pose animation is read-only for a moment");
            }
            ImGui::BeginDisabled(locked);
            drawBody();
            ImGui::EndDisabled();
        }
        drawMappingPopup();
        drawOpenPopup();
        drawRenamePopup();
        drawTemplatePopups();
        if (windowFocused) handleLocalKeys();
    }else{
        processPendingOpen();
    }
    ImGui::End();
    drawSceneKeysHint();
    if (!open){
        setOpen(false);
    }
}

void editor::PoseAnimationWindow::drawBody(){
    // Stacked (transport, strip, timing, tabs) when there is height for it or the window is
    // narrow; wide and short (the default bottom dock): two columns, the strip and timing on
    // the left with thumbnails sized to the height, the tabs on the right with their own scroll
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float line = ImGui::GetFrameHeightWithSpacing();
    const float text = ImGui::GetTextLineHeight();
    const float cardChrome = Theme::dpi(16.0f) + text * 2;                       // card minus its thumbnail
    const float stripChrome = style.ScrollbarSize + Theme::dpi(8.0f) + style.ItemSpacing.y;
    const float timing = line + Theme::dpi(TIMING_HEIGHT) + style.ItemSpacing.y;
    const float globalBar = session.globalEditActive() ? line + Theme::dpi(8.0f) : 0.0f;
    const float stackNeeds = line * 3 + globalBar + cardChrome + Theme::dpi(THUMB_HEIGHT) + stripChrome + timing + Theme::dpi(220.0f);
    compactLayout = avail.x >= Theme::dpi(1050.0f) && avail.y < stackNeeds;
    if (!compactLayout){
        thumbHeight = THUMB_HEIGHT;
        drawTransport();
        drawGlobalEditBar();
        drawStrip();
        drawTimingBar();
        drawTabs();
        return;
    }
    float left = std::max(Theme::dpi(560.0f), avail.x * 0.6f);
    if (avail.x - left - style.ItemSpacing.x < Theme::dpi(340.0f)) left = avail.x - Theme::dpi(340.0f) - style.ItemSpacing.x;
    // two transport rows (the second has Bake, the status line and Global Edit)
    const float forThumb = avail.y - line * 2 - globalBar - cardChrome - stripChrome - timing - Theme::dpi(6.0f);
    thumbHeight = std::clamp(forThumb / Theme::dpi(1.0f), 64.0f, THUMB_HEIGHT);
    ImGui::BeginChild("##PoseLeft", ImVec2(left, avail.y));
    drawTransport();
    drawGlobalEditBar();
    drawStrip();
    drawTimingBar();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##PoseRight", ImVec2(0, avail.y));
    drawTabs();
    ImGui::EndChild();
}

// ---- setup row ----

void editor::PoseAnimationWindow::drawSetupRow(){
    // one row when the window is wide (the bottom dock is short), else two
    const bool wide = ImGui::GetContentRegionAvail().x > Theme::dpi(1000.0f);
    drawCharacterField();
    ImGui::SameLine();
    // the chip wraps to its own line in a narrow window
    if (ImGui::GetContentRegionAvail().x < Theme::dpi(230.0f)) ImGui::NewLine();
    drawRigChip();
    if (wide) ImGui::SameLine(0.0f, Theme::dpi(24.0f));
    drawAnimationField(wide);
}

void editor::PoseAnimationWindow::drawCharacterField(){
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Character");
    ImGui::SameLine(Theme::dpi(78.0f));
    const std::string name = characterName();
    const float buttonWidth = ImGui::CalcTextSize(ICON_FA_CROSSHAIRS " Use Selection").x + ImGui::GetStyle().FramePadding.x * 2 +
                              ImGui::GetStyle().ItemSpacing.x;
    const float width = std::clamp(ImGui::GetContentRegionAvail().x - buttonWidth, Theme::dpi(60.0f), Theme::dpi(220.0f));
    const std::string label = (name.empty() ? std::string("None (Model)") : std::string(ICON_FA_PERSON " ") + name) + "###PoseCharacter";
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    if (ImGui::Button(label.c_str(), ImVec2(width, 0)) && session.bound()){
        // select the character in the Hierarchy, as an object field pings its target
        project->setSelectedEntity(session.sceneId(), session.model());
    }
    ImGui::PopStyleVar();
    if (ImGui::BeginDragDropTarget()){
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("entity")){
            const EntityPayload* entityPayload = static_cast<const EntityPayload*>(payload->Data);
            const uint32_t sceneId = payloadSceneId(*entityPayload, project->getSelectedSceneId());
            std::string error;
            if (bindCharacter(sceneId, entityPayload->entity, &error)){
                if (data() && !data()->poses.empty()) session.startPosing();
            }else{
                setStatus(error, true);
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SetItemTooltip("The scene Model GameObject to pose (drag one from the Hierarchy).\nClick to select it.");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_CROSSHAIRS " Use Selection")){
        useSelection();
    }
    ImGui::SetItemTooltip("Pose the GameObject selected in the Hierarchy or the Scene view");
}

void editor::PoseAnimationWindow::drawRigChip(){
    const std::shared_ptr<const PoseRigInfo> info = session.bound() ? session.rigInfo() : candidateInfo;
    if (!info && candidateError.empty()){
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        return;
    }
    poseui::RigStatus status;
    if (info){
        status = poseui::rigStatus(info->humanoid.bones);
        if (!session.bound() && !status.warning){
            status.warning = true;   // mapped, yet the rig could not be built
            status.tooltip = candidateError;
        }
    }else{
        status.text = "Not a humanoid";
        status.warning = true;
        status.tooltip = candidateError;
    }
    ImGui::AlignTextToFramePadding();
    if (status.warning){
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s", status.text.c_str());
    }else{
        ImGui::TextColored(Theme::Colors::SubtleText, ICON_FA_BONE " %s", status.text.c_str());
    }
    ImGui::SetItemTooltip("%s", status.tooltip.c_str());
    if (info){
        ImGui::SameLine();
        if (ImGui::SmallButton("Map Bones...")){
            openMappingPopup(session.bound() ? session.sceneId() : candidateScene, session.bound() ? session.model() : candidateEntity);
        }
        ImGui::SetItemTooltip("Choose which skeleton bone drives each humanoid slot (writes the model's .humanoid file)");
        ImGui::SameLine();
        if (ImGui::SmallButton("Avatar...")){
            Backend::getApp().openAvatarWindow(session.bound() ? session.sceneId() : candidateScene, session.bound() ? session.model() : candidateEntity);
        }
        ImGui::SetItemTooltip("Configure Avatar: the bone map, fingers, eyes and jaw, twist corrections and a retargeted test clip");
    }
}

void editor::PoseAnimationWindow::drawAnimationField(bool inlineRow){
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Animation");
    if (inlineRow) ImGui::SameLine();
    else ImGui::SameLine(Theme::dpi(78.0f));
    const auto& doc = session.document();
    std::string label = doc ? std::string(ICON_FA_PERSON " ") + editor::FileUtils::pathToUtf8(editor::FileUtils::pathFromUtf8(doc->path()).filename()) : std::string("None (Pose Animation)");
    if (doc && !doc->exists()) label += " (deleted)";
    label += "###PoseAnimationAsset";
    const float buttons = ImGui::CalcTextSize(ICON_FA_FILE_CIRCLE_PLUS " New" ICON_FA_FOLDER_OPEN " Open").x +
                          ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x * 2;
    const float width = std::clamp(ImGui::GetContentRegionAvail().x - buttons, Theme::dpi(60.0f), Theme::dpi(220.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    ImGui::Button(label.c_str(), ImVec2(width, 0));
    ImGui::PopStyleVar();
    if (ImGui::BeginDragDropTarget()){
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
            std::vector<std::string> files = Util::getStringsFromPayload(payload);
            for (const std::string& file : files){
                if (Util::isPoseAnimationFile(file)){
                    std::string error;
                    if (!openAnimation(file, &error)) setStatus(error, true);
                    break;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (doc) ImGui::SetItemTooltip("%s", doc->path().c_str());
    else ImGui::SetItemTooltip("The .poseanim asset to edit (drag one from the Project window)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!session.bound());
    if (ImGui::Button(ICON_FA_FILE_CIRCLE_PLUS " New")){
        std::string error;
        if (!createAnimation(&error)) setStatus(error, true);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("A new pose animation next to the character's model file: one neutral pose, Snappy feel");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " Open")){
        openPopupRequested = true;
    }
    ImGui::SetItemTooltip("Open a .poseanim of the project");
}

// ---- empty states ----

bool editor::PoseAnimationWindow::drawEmptyState(){
    emptyState.clear();
    const auto& doc = session.document();
    const pose::PoseAnimationData* animation = data();
    std::string message;
    std::function<void()> action;
    if (!session.bound() && candidateEntity != NULL_ENTITY){
        emptyState = "not_humanoid";
        message = "This model's bones could not be mapped to a humanoid.";
        if (!candidateError.empty()) message += "\n" + candidateError;
    }else if (!session.bound()){
        emptyState = "no_character";
        message = "Select a character in the scene";
    }else if (!doc || !doc->exists()){
        emptyState = "no_animation";
        message = "Create or open a pose animation for " + characterName();
    }else if (animation && animation->poses.empty()){
        emptyState = "no_poses";
        message = "This animation has no poses yet";
    }else{
        return false;
    }

    // no character, no animation or no poses: the templates are a way to start (phase 6)
    const bool withTemplates = emptyState != "not_humanoid";
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::Dummy(ImVec2(0, withTemplates ? Theme::dpi(6.0f) : std::max(0.0f, avail.y * 0.3f - Theme::dpi(30.0f))));
    const float wrap = std::min(avail.x - Theme::dpi(20.0f), Theme::dpi(420.0f));
    const ImVec2 textSize = ImGui::CalcTextSize(message.c_str(), nullptr, false, wrap);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - textSize.x) * 0.5f));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubtleText);
    ImGui::TextUnformatted(message.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    auto centredButton = [&](const char* label, float width){
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (avail.x - width) * 0.5f + ImGui::GetStyle().WindowPadding.x));
        return ImGui::Button(label, ImVec2(width, 0));
    };
    const float width = Theme::dpi(150.0f);
    if (emptyState == "no_character"){
        if (centredButton(ICON_FA_CROSSHAIRS " Use Selection", width)) useSelection();
    }else if (emptyState == "not_humanoid"){
        if (candidateInfo && centredButton("Map Bones...", width)) openMappingPopup(candidateScene, candidateEntity);
    }else if (emptyState == "no_animation"){
        const float pair = width * 2 + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (avail.x - pair) * 0.5f + ImGui::GetStyle().WindowPadding.x));
        if (ImGui::Button(ICON_FA_FILE_CIRCLE_PLUS " New", ImVec2(width, 0))){
            std::string error;
            if (!createAnimation(&error)) setStatus(error, true);
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FOLDER_OPEN " Open", ImVec2(width, 0))) openPopupRequested = true;
    }else if (emptyState == "no_poses"){
        if (centredButton(ICON_FA_PLUS " Add pose", width)) actionAdd();
    }
    if (!statusLine.empty()){
        ImGui::Spacing();
        const ImVec2 size = ImGui::CalcTextSize(statusLine.c_str(), nullptr, false, wrap);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - size.x) * 0.5f));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
        ImGui::TextColored(statusError ? Theme::Colors::ErrorText : Theme::Colors::SubtleText, "%s", statusLine.c_str());
        ImGui::PopTextWrapPos();
    }
    if (withTemplates){
        ImGui::Spacing();
        ImGui::SeparatorText(emptyState == "no_character" ? "Templates" : "Start from a template");
        ImGui::BeginChild("##EmptyTemplates");
        drawTemplatesTab();
        ImGui::EndChild();
    }
    return true;
}

// ---- transport ----

void editor::PoseAnimationWindow::drawTransport(){
    // a snapshot: an edit committed below replaces the document's data, not this
    const std::shared_ptr<const pose::PoseAnimationData> held = view();
    const pose::PoseAnimationData* animation = held.get();
    if (!animation) return;
    const bool previewing = session.mode() == PoseEditSession::Mode::Previewing;
    const bool playing = session.previewPlaying();

    if (ImGui::Button(playing ? ICON_FA_PAUSE "###PosePlay" : ICON_FA_PLAY "###PosePlay")){
        if (playing) pause(); else play();
    }
    ImGui::SetItemTooltip(playing ? "Pause (Space)" : "Play the preview: the frames that bake (Space)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!previewing);
    if (ImGui::Button(ICON_FA_STOP "###PoseStop")) stop();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Stop and pose the selected pose again (Escape)");
    ImGui::SameLine();

    const float length = session.previewLength();
    float time = session.previewTime();
    const std::string label = poseui::transportLabel(length, static_cast<int>(animation->poses.size()), previewing, time,
                                                     session.previewFrame());
    const float labelWidth = ImGui::CalcTextSize("00.00 / 00.00 s \xC2\xB7 frame 0000").x;
    const float bakeWidth = ImGui::CalcTextSize(ICON_FA_FILM " Bake " ICON_FA_CARET_DOWN).x + ImGui::GetStyle().FramePadding.x * 2 + Theme::dpi(150.0f);
    const bool oneRow = !compactLayout && ImGui::GetContentRegionAvail().x > Theme::dpi(900.0f) + bakeWidth;
    const bool narrow = ImGui::GetContentRegionAvail().x < labelWidth + Theme::dpi(320.0f);
    const float rightWidth = labelWidth + Theme::dpi(200.0f) + (oneRow ? bakeWidth + Theme::dpi(16.0f) : 0.0f);
    ImGui::SetNextItemWidth(narrow ? -FLT_MIN : std::max(Theme::dpi(60.0f), ImGui::GetContentRegionAvail().x - rightWidth));
    if (ImGui::SliderFloat("##PoseScrub", &time, 0.0f, std::max(length, 0.001f), "", ImGuiSliderFlags_AlwaysClamp)){
        if (!previewing) session.startPreview();
        session.setPreviewPlaying(false);
        session.setPreviewTime(time);
    }
    ImGui::SetItemTooltip("Scrub the preview (, and . step one frame)");
    if (!narrow) ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label.c_str());
    ImGui::SameLine();

    bool loop = animation->settings.loop;
    if (ImGui::Checkbox("Loop", &loop)){
        pose::PoseAnimationData edited = *animation;
        edited.settings.loop = loop;
        commit(edited, "Loop");
    }
    ImGui::SetItemTooltip("Loop back to the first pose (off: one-shot, the preview replays after a pause)");
    ImGui::SameLine();
    static const int rates[] = {24, 30, 60};
    std::string rateLabel = std::to_string(animation->settings.frameRate) + " fps";
    ImGui::SetNextItemWidth(Theme::dpi(66.0f));
    if (ImGui::BeginCombo("##PoseFps", rateLabel.c_str())){
        for (int rate : rates){
            if (ImGui::Selectable((std::to_string(rate) + " fps").c_str(), animation->settings.frameRate == rate)){
                pose::PoseAnimationData edited = *animation;
                edited.settings.frameRate = rate;
                commit(edited, "Frame rate");
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Frames per second of the preview, the bake and runtime playback");

    // bake: on the same row when there is room
    if (oneRow) ImGui::SameLine(0.0f, Theme::dpi(16.0f));
    // Bake: to a shared Animation Clip (primary) or into the scene (the entity Animation)
    if (ImGui::Button(ICON_FA_FILM " Bake " ICON_FA_CARET_DOWN)) ImGui::OpenPopup("##PoseBakeMenu");
    ImGui::SetItemTooltip("Bake the frames of the preview to an Animation Clip (.anim, shared) or into the scene");
    if (ImGui::BeginPopup("##PoseBakeMenu")){
        if (ImGui::MenuItem(ICON_FA_FILM " Bake to Animation Clip")){
            const auto& doc = session.document();
            if (!doc || !session.bound()){
                setStatus("Pick a character and an animation to bake", true);
            }else{
                PoseClipBakeResult result;
                std::string error;
                const std::string name = bakeName.empty() ? fileStem(doc->path()) : bakeName;
                if (!PoseBaker::bakeToClip(project, session.sceneId(), session.model(), *doc, name, "", result, error)){
                    setStatus("Bake failed: " + error, true);
                }else{
                    setStatus("Baked " + std::to_string(result.frames) + " frames to '" + result.clip + "' (state '" + result.name +
                              "' of the Animation Player on " + characterName() + ")", false);
                }
            }
        }
        ImGui::SetItemTooltip("Writes <name>.anim next to the pose animation (baking again updates it) and lists it in the\n"
                              "character's Animation Player: one clip any character with the same bones can play.");
        if (ImGui::MenuItem(ICON_FA_PERSON " Bake to Humanoid Clip")){
            const auto& doc = session.document();
            if (!doc || !session.bound()){
                setStatus("Pick a character and an animation to bake", true);
            }else{
                PoseClipBakeResult result;
                std::string error;
                const std::string name = bakeName.empty() ? fileStem(doc->path()) : bakeName;
                if (!PoseBaker::bakeToClip(project, session.sceneId(), session.model(), *doc, name, "", result, error, true)){
                    setStatus("Bake failed: " + error, true);
                }else{
                    setStatus("Baked " + std::to_string(result.frames) + " frames to the humanoid clip '" + result.clip + "' (state '" + result.name +
                              "' of the Animation Player on " + characterName() + ")", false);
                }
            }
        }
        ImGui::SetItemTooltip("Writes <name> (Humanoid).anim: the poses' joint rotations, hips and foot goals, a clip any\n"
                              "humanoid character plays (retargeted), listed in the character's Animation Player.");
        if (ImGui::MenuItem(ICON_FA_DIAGRAM_PROJECT " Bake into Scene")) bake();
        ImGui::SetItemTooltip("Bake to an Animation of the character in this scene (baking again updates it).\n"
                              "Plays with Model:playAnimation(name) and in the Animation window.");
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    char nameBuffer[128];
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", bakeName.c_str());
    ImGui::SetNextItemWidth(Theme::dpi(140.0f));
    if (ImGui::InputTextWithHint("##PoseBakeName", "animation name", nameBuffer, sizeof(nameBuffer))){
        bakeName = nameBuffer;
    }
    ImGui::SetItemTooltip("The Animation's name (default: the asset's file name)");
    if (compactLayout){
        ImGui::SameLine();
        drawStatusAndGlobalEditTool();
    }
}

// ---- Global Edit bar ----

void editor::PoseAnimationWindow::drawStatusAndGlobalEditTool(){
    // the status line (bake result, errors), then the Global Edit tool at the right of the line
    const bool active = session.globalEditActive();
    const float toolWidth = ImGui::CalcTextSize(ICON_FA_LAYER_GROUP " Global Edit").x + ImGui::GetStyle().FramePadding.x * 2;
    if (!statusLine.empty()){
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Theme::dpi(8.0f));
        const float room = ImGui::GetContentRegionAvail().x - toolWidth - Theme::dpi(12.0f);
        if (room > Theme::dpi(40.0f)){
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, statusError ? Theme::Colors::ErrorText : Theme::Colors::SubtleText);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::PushClipRect(at, ImVec2(at.x + room, at.y + ImGui::GetFrameHeight()), true);
            ImGui::TextUnformatted(statusLine.c_str());
            ImGui::PopClipRect();
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", statusLine.c_str());
            ImGui::SameLine();
        }
    }
    rightAlign(toolWidth);
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, GLOBAL_EDIT_ORANGE);
    if (active) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.1f, 0.1f, 0.1f, 1.0f));
    if (ImGui::Button(ICON_FA_LAYER_GROUP " Global Edit")) actionGlobalEdit();
    if (active) ImGui::PopStyleColor(2);
    ImGui::SetItemTooltip("Change this pose, then apply the same change to the other poses (E)");
}

void editor::PoseAnimationWindow::drawGlobalEditBar(){
    // a snapshot: an edit committed below replaces the document's data, not this
    const std::shared_ptr<const pose::PoseAnimationData> held = view();
    const pose::PoseAnimationData* animation = held.get();
    if (!animation) return;
    // the strip header (stacked layout): its title, the status line and the Global Edit tool;
    // the compact layout has them in the transport's second row
    if (!compactLayout){
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Theme::Colors::SubtleText, "POSES");
        ImGui::SameLine();
        drawStatusAndGlobalEditTool();
    }
    if (!session.globalEditActive()) return;

    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.y * 2;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(start, ImVec2(start.x + width, start.y + height), color(GLOBAL_EDIT_ORANGE, 0.16f), 3.0f);
    draw->AddRect(start, ImVec2(start.x + width, start.y + height), color(GLOBAL_EDIT_ORANGE), 3.0f);
    ImGui::SetCursorScreenPos(ImVec2(start.x + ImGui::GetStyle().FramePadding.x, start.y + ImGui::GetStyle().FramePadding.y));
    const int changed = session.globalEditChangedSlots();
    const float buttons = ImGui::CalcTextSize("Apply to AllApply to SelectedCancel").x + ImGui::GetStyle().FramePadding.x * 6 +
                          ImGui::GetStyle().ItemSpacing.x * 2 + Theme::dpi(8.0f);
    ImGui::AlignTextToFramePadding();
    {
        // the message gives way to the buttons in a narrow column
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float room = std::max(0.0f, width - buttons - ImGui::GetStyle().FramePadding.x * 2);
        ImGui::PushClipRect(at, ImVec2(at.x + room, at.y + ImGui::GetFrameHeight()), true);
        ImGui::TextColored(GLOBAL_EDIT_ORANGE, "%s", changed == 0 ? "Global Edit: nothing changed yet. Change this pose, then apply the change to other poses."
                                                                 : "Global Edit: change this pose, then apply the same change to other poses");
        ImGui::PopClipRect();
    }
    ImGui::SameLine();
    rightAlign(buttons);
    ImGui::BeginDisabled(changed == 0);
    if (ImGui::Button("Apply to All")){
        if (!session.applyGlobalEdit({})) setStatus("Global Edit: nothing to apply", true);
    }
    ImGui::SameLine();
    std::vector<int> others;
    for (int i : selection()){
        if (i != session.primary()) others.push_back(i);
    }
    ImGui::BeginDisabled(others.empty());
    if (ImGui::Button("Apply to Selected")){
        if (!session.applyGlobalEdit(others)) setStatus("Global Edit: nothing to apply", true);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Ctrl/Cmd-click other poses in the strip to pick them");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) session.cancelGlobalEdit();
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + height + ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(0, 0));
}

// ---- strip ----

void editor::PoseAnimationWindow::drawStrip(){
    // a snapshot: an edit committed below replaces the document's data, not this
    const std::shared_ptr<const pose::PoseAnimationData> held = view();
    const pose::PoseAnimationData* animation = held.get();
    if (!animation) return;
    const int count = static_cast<int>(animation->poses.size());
    // thumbnails keep 88:116; the card stays wide enough for the name
    const float cardWidth = Theme::dpi(std::max(CARD_WIDTH * thumbHeight / THUMB_HEIGHT, 80.0f));
    const float chipWidth = Theme::dpi(CHIP_WIDTH);
    const float lineHeight = ImGui::GetTextLineHeight();
    const float cardHeight = Theme::dpi(6.0f) + Theme::dpi(thumbHeight) + Theme::dpi(4.0f) + lineHeight * 2 + Theme::dpi(6.0f);
    const float stripHeight = cardHeight + ImGui::GetStyle().ScrollbarSize + Theme::dpi(6.0f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::ToolbarBackground);
    ImGui::BeginChild("##PoseStrip", ImVec2(0, stripHeight), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    // the wheel scrolls the strip sideways
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f && !ImGui::GetIO().KeyCtrl){
        ImGui::SetScrollX(ImGui::GetScrollX() - ImGui::GetIO().MouseWheel * cardWidth * 0.75f);
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    std::vector<float> centres;
    float x = origin.x + Theme::dpi(4.0f);
    const float y = origin.y + Theme::dpi(2.0f);
    for (int i = 0; i < count; i++){
        drawCard(i, ImVec2(x, y), cardWidth, cardHeight);
        centres.push_back(x + cardWidth * 0.5f);
        x += cardWidth;
        const bool hasChip = i < count - 1 || animation->settings.loop;
        if (hasChip && count > 1){
            drawChip(i, ImVec2(x, y), chipWidth, cardHeight);
        }
        x += chipWidth;
    }
    // the + card: duplicates the primary pose after it
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushID("PoseAddCard");
    if (ImGui::InvisibleButton("##add", ImVec2(cardWidth, cardHeight))) actionAdd();
    const bool addHovered = ImGui::IsItemHovered();
    ImGui::SetItemTooltip("Duplicate the selected pose after it (N)");
    ImGui::PopID();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 addMin(x + Theme::dpi(6.0f), y + Theme::dpi(6.0f));
    const ImVec2 addMax(x + cardWidth - Theme::dpi(6.0f), y + cardHeight - Theme::dpi(6.0f));
    draw->AddRect(addMin, addMax, color(Theme::Colors::SubtleText, addHovered ? 0.9f : 0.45f), 4.0f, 0, Theme::dpi(1.5f));
    // a big plus drawn with lines, the label under it
    const ImVec2 centre((addMin.x + addMax.x) * 0.5f, (addMin.y + addMax.y) * 0.5f - Theme::dpi(8.0f));
    const float arm = std::min(Theme::dpi(14.0f), (addMax.x - addMin.x) * 0.22f);
    const ImU32 plusColor = color(Theme::Colors::SubtleText, addHovered ? 1.0f : 0.7f);
    draw->AddLine(ImVec2(centre.x - arm, centre.y), ImVec2(centre.x + arm, centre.y), plusColor, Theme::dpi(3.0f));
    draw->AddLine(ImVec2(centre.x, centre.y - arm), ImVec2(centre.x, centre.y + arm), plusColor, Theme::dpi(3.0f));
    const char* addText = "Add pose";
    const ImVec2 addTextSize = ImGui::CalcTextSize(addText);
    draw->AddText(ImVec2((addMin.x + addMax.x - addTextSize.x) * 0.5f, centre.y + arm + Theme::dpi(6.0f)), color(Theme::Colors::SubtleText), addText);
    x += cardWidth + Theme::dpi(4.0f);
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(x - origin.x, cardHeight + Theme::dpi(2.0f)));

    // card drags: a press becomes a reorder after 6 px, with an insertion marker
    if (pressedCard >= 0){
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (!draggingCards && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
            poseui::dragStarted(mouse.x - pressPosition.x, mouse.y - pressPosition.y)){
            draggingCards = true;
            if (!poseui::isSelected(poseui::Selection{session.primary(), session.primary(), selection()}, pressedCard)){
                session.select(pressedCard, false, false);
            }
        }
        if (draggingCards){
            dropIndex = poseui::insertionIndexAt(centres, mouse.x);
            const float markerX = dropIndex < count ? centres[dropIndex] - cardWidth * 0.5f - chipWidth * 0.5f
                                                    : (count > 0 ? centres.back() + cardWidth * 0.5f + chipWidth * 0.5f : origin.x);
            draw->AddLine(ImVec2(markerX, y), ImVec2(markerX, y + cardHeight), color(Theme::Colors::PrefabText), Theme::dpi(3.0f));
            ImGui::SetTooltip("%s", poseui::poseCountLabel("Move", static_cast<int>(selection().size())).c_str());
            // scroll near the edges
            const ImVec2 windowPos = ImGui::GetWindowPos();
            const float windowWidth = ImGui::GetWindowWidth();
            if (mouse.x < windowPos.x + Theme::dpi(30.0f)) ImGui::SetScrollX(ImGui::GetScrollX() - Theme::dpi(8.0f));
            if (mouse.x > windowPos.x + windowWidth - Theme::dpi(30.0f)) ImGui::SetScrollX(ImGui::GetScrollX() + Theme::dpi(8.0f));
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)){
            if (draggingCards){
                if (dropIndex >= 0) actionMoveTo(dropIndex);
            }else{
                const ImGuiIO& io = ImGui::GetIO();
                actionSelect(pressedCard, io.KeyCtrl || io.KeySuper, io.KeyShift);
            }
            pressedCard = -1;
            draggingCards = false;
            dropIndex = -1;
        }
    }
    drawCardMenu();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void editor::PoseAnimationWindow::drawCard(int index, const ImVec2& pos, float cardWidth, float cardHeight){
    // a snapshot: an edit committed below replaces the document's data, not this
    const std::shared_ptr<const pose::PoseAnimationData> held = view();
    const pose::PoseAnimationData* animation = held.get();
    if (!animation || index >= static_cast<int>(animation->poses.size())) return;
    const pose::PoseKey& key = animation->poses[index];
    const bool selected = std::find(selection().begin(), selection().end(), index) != selection().end();
    const bool primary = index == session.primary();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    ImGui::SetCursorScreenPos(pos);
    ImGui::PushID(index);
    ImGui::InvisibleButton("##card", ImVec2(cardWidth, cardHeight), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
        pressedCard = index;
        pressPosition = ImGui::GetIO().MousePos;
        draggingCards = false;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)){
        actionRename(index);
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)){
        if (!selected) actionSelect(index, false, false);
        contextPose = index;
        // opened by drawCardMenu: OpenPopup here would be in the card's ID scope, BeginPopup is not
        cardMenuRequested = true;
    }
    if (hovered && !draggingCards){
        ImGui::SetItemTooltip("%s\nClick: select  Shift: range  Ctrl/Cmd: toggle\nDrag: reorder  Double-click / F2: rename\nRight-click: more",
                              key.name.c_str());
    }
    ImGui::PopID();
    if (scrollToPrimary && primary){
        ImGui::SetScrollHereX(0.5f);
        scrollToPrimary = false;
    }

    const ImVec2 cardMin(pos.x + Theme::dpi(2.0f), pos.y + Theme::dpi(2.0f));
    const ImVec2 cardMax(pos.x + cardWidth - Theme::dpi(2.0f), pos.y + cardHeight - Theme::dpi(2.0f));
    const ImVec4 background = selected ? Theme::Colors::Selection : (hovered ? Theme::Colors::FileCardBackgroundHovered : Theme::Colors::FileCardBackground);
    draw->AddRectFilled(cardMin, cardMax, color(background), 4.0f);
    if (primary){
        draw->AddRect(cardMin, cardMax, color(Theme::Colors::PrefabText), 4.0f, 0, Theme::dpi(2.0f));
    }
    if (draggingCards && selected){
        draw->AddRectFilled(cardMin, cardMax, IM_COL32(0, 0, 0, 90), 4.0f);
    }

    // the thumbnail: the primary card is live while posing
    const float thumbWidth = thumbHeight * THUMB_WIDTH / THUMB_HEIGHT;
    const ImVec2 thumbMin(pos.x + (cardWidth - Theme::dpi(thumbWidth)) * 0.5f, pos.y + Theme::dpi(6.0f));
    const ImVec2 thumbMax(thumbMin.x + Theme::dpi(thumbWidth), thumbMin.y + Theme::dpi(thumbHeight));
    const int width = static_cast<int>(std::lround(Theme::dpi(thumbWidth)));
    const int height = static_cast<int>(std::lround(Theme::dpi(thumbHeight)));
    TextureRender* texture = nullptr;
    // cards scrolled out of the strip request nothing
    if (session.bound() && ImGui::IsRectVisible(thumbMin, thumbMax)){
        if (primary && session.mode() == PoseEditSession::Mode::Posing){
            const pose::HumanPose& working = session.working();
            texture = liveThumbnails.get(PoseThumbnails::hashPose(working), working, width, height, 0);
            if (!texture){
                texture = thumbnails.get(PoseThumbnails::hashKey(key) ^ (animation->settings.humanize.jointLimits ? 1u : 0u),
                                         session.keyPose(index), width, height, index);
            }
        }else{
            texture = thumbnails.get(PoseThumbnails::hashKey(key) ^ (animation->settings.humanize.jointLimits ? 1u : 0u),
                                     session.keyPose(index), width, height, index);
        }
    }
    draw->AddRectFilled(thumbMin, thumbMax, IM_COL32(48, 48, 51, 255), 3.0f);
    if (texture){
        draw->AddImage(Backend::getImGuiTexture(texture), thumbMin, thumbMax);
    }else{
        const char* icon = ICON_FA_PERSON;
        const ImVec2 size = ImGui::CalcTextSize(icon);
        draw->AddText(ImVec2((thumbMin.x + thumbMax.x - size.x) * 0.5f, (thumbMin.y + thumbMax.y - size.y) * 0.5f),
                      color(Theme::Colors::SubtleText, 0.6f), icon);
    }
    // index badge
    const std::string number = std::to_string(index);
    const ImVec2 numberSize = ImGui::CalcTextSize(number.c_str());
    const ImVec2 badgeMin(thumbMin.x + Theme::dpi(3.0f), thumbMin.y + Theme::dpi(3.0f));
    draw->AddRectFilled(badgeMin, ImVec2(badgeMin.x + numberSize.x + Theme::dpi(8.0f), badgeMin.y + numberSize.y + Theme::dpi(2.0f)),
                        IM_COL32(0, 0, 0, 150), 3.0f);
    draw->AddText(ImVec2(badgeMin.x + Theme::dpi(4.0f), badgeMin.y + Theme::dpi(1.0f)), IM_COL32(235, 235, 235, 255), number.c_str());
    if (!key.grounded){
        const char* lifted = ICON_FA_ARROW_DOWN;
        draw->AddText(ImVec2(thumbMax.x - Theme::dpi(16.0f), thumbMin.y + Theme::dpi(3.0f)), color(Theme::Colors::WarningText), lifted);
    }

    // name and hold
    const float textX = pos.x + Theme::dpi(7.0f);
    const float maxText = cardWidth - Theme::dpi(14.0f);
    std::string name = key.name.empty() ? std::string("(unnamed)") : key.name;
    while (name.size() > 1 && ImGui::CalcTextSize(name.c_str()).x > maxText){
        name.pop_back();
        while (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0) == 0x80) name.pop_back();   // whole UTF-8 characters
        if (ImGui::CalcTextSize((name + "..").c_str()).x <= maxText){
            name += "..";
            break;
        }
    }
    const float nameY = thumbMax.y + Theme::dpi(3.0f);
    draw->AddText(ImVec2(textX, nameY), color(ImGui::GetStyle().Colors[ImGuiCol_Text]), name.c_str());
    const std::string hold = poseui::holdLabel(key.hold);
    draw->AddText(ImVec2(textX, nameY + ImGui::GetTextLineHeight()), color(Theme::Colors::SubtleText), hold.c_str());
}

void editor::PoseAnimationWindow::drawChip(int index, const ImVec2& pos, float chipWidth, float cardHeight){
    // a snapshot: an edit committed below replaces the document's data, not this
    const std::shared_ptr<const pose::PoseAnimationData> held = view();
    const pose::PoseAnimationData* animation = held.get();
    if (!animation || index >= static_cast<int>(animation->poses.size())) return;
    const pose::PoseKey& key = animation->poses[index];
    const pose::Transition& transition = pose::transitionOut(*animation, index);
    const bool loopBack = index == static_cast<int>(animation->poses.size()) - 1;
    const std::string text = std::string(loopBack ? ICON_FA_ARROW_ROTATE_LEFT : ICON_FA_ARROW_RIGHT) + " " + fixed(transition.duration, 2) + "s";
    const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
    const float chipHeight = textSize.y + Theme::dpi(6.0f);
    const ImVec2 chipMin(pos.x + (chipWidth - textSize.x) * 0.5f - Theme::dpi(5.0f), pos.y + Theme::dpi(6.0f) + Theme::dpi(thumbHeight) * 0.5f - chipHeight * 0.5f);
    const ImVec2 chipMax(chipMin.x + textSize.x + Theme::dpi(10.0f), chipMin.y + chipHeight);
    ImGui::SetCursorScreenPos(chipMin);
    ImGui::PushID(1000 + index);
    const bool clicked = ImGui::InvisibleButton("##chip", ImVec2(chipMax.x - chipMin.x, chipMax.y - chipMin.y));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::SetItemTooltip("%s of '%s': %s, %.2f s%s\nClick to edit it in the Pose tab", loopBack ? "Loop back" : "Transition",
                          key.name.c_str(), curveLabel(transition.curve), transition.duration,
                          key.customTransition ? " (custom)" : " (the animation's feel)");
    ImGui::PopID();
    if (clicked){
        actionSelect(index, false, false);
        selectTab("Pose");
        focusTransitionFields = true;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec4 accent = Theme::Colors::Selection;
    const ImVec4 fill = key.customTransition ? accent : Theme::Colors::FoldoutHeader;
    draw->AddRectFilled(chipMin, chipMax, color(fill, hovered ? 1.0f : 0.85f), chipHeight * 0.5f);
    draw->AddText(ImVec2(chipMin.x + Theme::dpi(5.0f), chipMin.y + Theme::dpi(3.0f)),
                  color(key.customTransition ? ImVec4(1, 1, 1, 1) : Theme::Colors::SubtleText), text.c_str());
    (void)cardHeight;
}

void editor::PoseAnimationWindow::drawCardMenu(){
    if (cardMenuRequested){
        cardMenuRequested = false;
        ImGui::OpenPopup("##PoseCardMenu");
    }
    if (!ImGui::BeginPopup("##PoseCardMenu")) return;
    menuShownFrame = ImGui::GetFrameCount();
    menuShown = "card";
    const int count = static_cast<int>(selection().size());
    if (ImGui::MenuItem(poseui::poseCountLabel("Duplicate", count).c_str(), shortcutText("pose.duplicate"))) actionDuplicate(false, false);
    if (ImGui::MenuItem(poseui::poseCountLabel("Duplicate", count).append(" at End").c_str())) actionDuplicate(true, false);
    if (ImGui::MenuItem(poseui::poseCountLabel("Duplicate", count).append(" Mirrored").c_str(), shortcutText("pose.duplicateMirrored"))) actionDuplicate(false, true);
    if (ImGui::MenuItem(poseui::poseCountLabel("Mirror", count).append(" (L \xE2\x86\x94 R)").c_str(), shortcutText("pose.mirror"))) actionMirror();
    ImGui::Separator();
    if (ImGui::MenuItem("Copy Pose", shortcutText("pose.copy"))) actionCopy();
    if (ImGui::MenuItem(poseui::poseCountLabel("Paste", count).c_str(), shortcutText("pose.paste"), false, clipboard.has_value())) actionPaste();
    ImGui::Separator();
    if (ImGui::MenuItem(poseui::poseCountLabel("Move", count).append(" Left").c_str(), shortcutText("pose.moveLeft"))) actionMove(-1);
    if (ImGui::MenuItem(poseui::poseCountLabel("Move", count).append(" Right").c_str(), shortcutText("pose.moveRight"))) actionMove(1);
    ImGui::Separator();
    if (ImGui::MenuItem("Rename", shortcutText("pose.rename"), false, count == 1)) actionRename(session.primary());
    if (ImGui::MenuItem(poseui::poseCountLabel("Reset", count).append(" to Neutral").c_str())) actionResetNeutral();
    if (ImGui::MenuItem(poseui::poseCountLabel("Delete", count).c_str(), shortcutText("pose.delete"))) actionDelete();
    ImGui::EndPopup();
}

void editor::PoseAnimationWindow::drawRenamePopup(){
    if (renameIndex >= 0 && !ImGui::IsPopupOpen("Rename Pose###PoseRename")){
        ImGui::OpenPopup("Rename Pose###PoseRename");
    }
    if (!ImGui::BeginPopup("Rename Pose###PoseRename")){
        renameIndex = -1;
        return;
    }
    if (renameFocus){
        ImGui::SetKeyboardFocusHere();
        renameFocus = false;
    }
    ImGui::SetNextItemWidth(Theme::dpi(200.0f));
    const bool enter = ImGui::InputText("##PoseRenameInput", renameBuffer, sizeof(renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    ImGui::SameLine();
    const bool ok = ImGui::Button("OK");
    if (enter || ok){
        const pose::PoseAnimationData* animation = data();
        if (animation && renameIndex >= 0 && renameIndex < static_cast<int>(animation->poses.size()) &&
            animation->poses[renameIndex].name != renameBuffer){
            pose::PoseAnimationData edited = *animation;
            edited.poses[renameIndex].name = renameBuffer;
            commit(edited, "Rename pose");
        }
        renameIndex = -1;
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)){
        renameIndex = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---- timing bar ----

void editor::PoseAnimationWindow::drawTimingBar(){
    // a snapshot: an edit committed below replaces the document's data, not this
    const std::shared_ptr<const pose::PoseAnimationData> held = view();
    const pose::PoseAnimationData* animation = held.get();
    if (!animation) return;
    const poseui::TimingLayout layout = poseui::timingLayout(*animation);
    if (layout.length <= 0.0) return;

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(Theme::Colors::SubtleText, "TIMING");
    ImGui::SameLine();
    ImGui::TextColored(Theme::Colors::SubtleText, "%s", fixed(layout.length, 2).append(" s").c_str());
    ImGui::SameLine();
    const float buttons = ImGui::CalcTextSize("Auto TimingReset Custom").x + ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x;
    rightAlign(buttons);
    if (ImGui::Button("Auto Timing")) actionAutoTiming();
    ImGui::SetItemTooltip("Holds and snaps from how much each pose moves, keeping the length");
    ImGui::SameLine();
    if (ImGui::Button("Reset Custom")) actionResetCustom();
    ImGui::SetItemTooltip("Every pose uses the animation's transition again (Feel tab)");

    const float barHeight = Theme::dpi(TIMING_HEIGHT);
    const float scrollbar = timingZoom > 1.0f ? ImGui::GetStyle().ScrollbarSize : 0.0f;
    ImGui::BeginChild("##PoseTiming", ImVec2(0, barHeight + scrollbar), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const float visibleWidth = ImGui::GetContentRegionAvail().x;
    const float pad = Theme::dpi(6.0f);
    const float barWidth = std::max(Theme::dpi(40.0f), (visibleWidth - pad * 2) * timingZoom);
    const bool dragging = dragEdge.kind != poseui::TimingHitKind::None;
    // the scale is frozen while an edge is dragged
    const double pps = dragging ? dragPixelsPerSecond : barWidth / layout.length;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float left = origin.x + pad;
    const float top = origin.y + Theme::dpi(4.0f);
    const float bottom = origin.y + barHeight - Theme::dpi(6.0f);
    ImGui::InvisibleButton("##timing", ImVec2(barWidth + pad * 2, barHeight));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(origin.x, origin.y), ImVec2(origin.x + barWidth + pad * 2, origin.y + barHeight), color(Theme::Colors::ToolbarBackground), 3.0f);

    auto xOf = [&](double t){ return static_cast<float>(left + t * pps); };
    // frame ticks when zoomed in
    const double tick = poseui::frameTickStep(pps, animation->settings.frameRate, Theme::dpi(6.0f));
    if (tick > 0.0){
        for (double t = 0.0; t <= layout.length + 1e-9; t += tick){
            draw->AddLine(ImVec2(xOf(t), bottom), ImVec2(xOf(t), bottom + Theme::dpi(3.0f)), color(Theme::Colors::SubtleText, 0.5f));
        }
    }
    // holds
    for (const poseui::TimingBlock& hold : layout.holds){
        const bool selected = std::find(selection().begin(), selection().end(), hold.pose) != selection().end();
        float x0 = xOf(hold.start);
        float x1 = xOf(hold.end);
        const bool marker = x1 - x0 < Theme::dpi(3.0f);
        if (marker){
            // a zero hold still shows (and stays draggable)
            const float mid = (x0 + x1) * 0.5f;
            x0 = mid - Theme::dpi(1.5f);
            x1 = mid + Theme::dpi(1.5f);
        }
        const ImVec4 fill = selected ? Theme::Colors::Selection : ImVec4(0.36f, 0.36f, 0.38f, 1.0f);
        draw->AddRectFilled(ImVec2(x0, top + Theme::dpi(8.0f)), ImVec2(x1, bottom), color(fill), 2.0f);
        if (hold.pose == session.primary()){
            draw->AddRect(ImVec2(x0, top + Theme::dpi(8.0f)), ImVec2(x1, bottom), color(Theme::Colors::PrefabText), 2.0f);
        }
        // the pose number at the bottom of its hold, under the settle of the curve that arrives;
        // a hold too narrow for it (a zero hold's marker): on a small tag above the hold
        const std::string number = std::to_string(hold.pose);
        const ImVec2 numberSize = ImGui::CalcTextSize(number.c_str());
        if (x1 - x0 > numberSize.x + Theme::dpi(4.0f)){
            draw->AddText(ImVec2(x0 + Theme::dpi(3.0f), bottom - ImGui::GetTextLineHeight() - Theme::dpi(1.0f)),
                          color(ImGui::GetStyle().Colors[ImGuiCol_Text], 0.8f), number.c_str());
        }else{
            const float mid = (x0 + x1) * 0.5f;
            const ImVec2 tagMin(mid - numberSize.x * 0.5f - Theme::dpi(2.0f), origin.y);
            const ImVec2 tagMax(mid + numberSize.x * 0.5f + Theme::dpi(2.0f), origin.y + numberSize.y);
            draw->AddRectFilled(tagMin, tagMax, color(marker ? fill : Theme::Colors::ToolbarBackground, 0.9f), 2.0f);
            draw->AddText(ImVec2(mid - numberSize.x * 0.5f, origin.y), color(ImGui::GetStyle().Colors[ImGuiCol_Text], 0.9f), number.c_str());
        }
    }
    // transitions as their ease curve, continued over the next hold so overshoot shows
    for (size_t s = 0; s < layout.transitions.size(); s++){
        const poseui::TimingBlock& block = layout.transitions[s];
        const pose::Transition& curve = layout.curves[s];
        const double duration = std::max(1e-4, block.end - block.start);
        const int next = block.loopBack ? 0 : block.pose + 1;
        const double nextHold = next < static_cast<int>(layout.holds.size()) ? layout.holds[next].end - layout.holds[next].start : 0.0;
        const float xMax = static_cast<float>(std::clamp(1.0 + nextHold / duration, 1.0, 2.0));
        const int samples = static_cast<int>(std::ceil(24 * xMax));
        const std::vector<float> values = poseui::easeSamples(curve, samples, xMax);
        const ImVec4 lineColor = block.custom ? Theme::Colors::PrefabText : ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
        const float span = (bottom - top) * 0.62f;
        const float base = bottom - Theme::dpi(2.0f);
        std::vector<ImVec2> points;
        for (int i = 0; i < samples; i++){
            const double x = static_cast<double>(xMax) * i / (samples - 1);
            const double t = block.start + x * duration;
            if (block.loopBack && t > layout.length) break;
            points.push_back(ImVec2(xOf(t), base - values[i] * span));
        }
        draw->AddRectFilled(ImVec2(xOf(block.start), top + Theme::dpi(8.0f)), ImVec2(xOf(block.end), bottom),
                            color(lineColor, 0.08f), 2.0f);
        if (points.size() >= 2){
            draw->AddPolyline(points.data(), static_cast<int>(points.size()), color(lineColor, 0.95f), ImDrawFlags_None, Theme::dpi(1.5f));
        }
    }
    // playhead
    if (session.mode() == PoseEditSession::Mode::Previewing){
        const float x = xOf(session.previewTime());
        draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + barHeight), color(PLAYHEAD_COLOR), Theme::dpi(2.0f));
    }

    // interaction
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const poseui::TimingHit hover = poseui::timingHitTest(layout, mouse.x - left, pps, Theme::dpi(5.0f));
    const bool edgeHover = hover.kind == poseui::TimingHitKind::HoldEdge || hover.kind == poseui::TimingHitKind::TransitionEdge;
    if ((hovered && edgeHover) || dragging){
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (hovered && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0.0f){
        timingZoom = std::clamp(timingZoom * (ImGui::GetIO().MouseWheel > 0 ? 1.25f : 0.8f), 1.0f, 40.0f);
    }else if (hovered && ImGui::GetIO().MouseWheel != 0.0f && timingZoom > 1.0f){
        ImGui::SetScrollX(ImGui::GetScrollX() - ImGui::GetIO().MouseWheel * Theme::dpi(40.0f));
    }
    if (hovered && !dragging && edgeHover){
        const float value = poseui::timingEdgeValue(*animation, hover);
        ImGui::SetTooltip("%s of '%s': %.3f s\nDrag to change (snaps to frames; Alt: free)\nCtrl+wheel: zoom",
                          hover.kind == poseui::TimingHitKind::HoldEdge ? "Hold" : "Transition", animation->poses[hover.pose].name.c_str(), value);
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
        if (edgeHover){
            dragEdge = hover;
            dragStartValue = poseui::timingEdgeValue(*animation, hover);
            dragStartX = mouse.x;
            dragPixelsPerSecond = pps;
            dragSerial++;
        }else if (hover.pose >= 0){
            actionSelect(hover.pose, ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper, ImGui::GetIO().KeyShift);
        }
    }
    if (dragging){
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)){
            pose::PoseAnimationData edited = *animation;
            const float delta = static_cast<float>((mouse.x - dragStartX) / dragPixelsPerSecond);
            if (poseui::applyTimingDrag(edited, dragEdge, dragStartValue, delta, !ImGui::GetIO().KeyAlt)){
                const bool transition = dragEdge.kind == poseui::TimingHitKind::TransitionEdge;
                commit(edited, transition ? "Transition time" : "Hold time", "timing#" + std::to_string(dragSerial));
            }
            const pose::PoseAnimationData* now = data();
            if (now){
                ImGui::SetTooltip("%s: %.3f s", dragEdge.kind == poseui::TimingHitKind::HoldEdge ? "Hold" : "Transition",
                                  poseui::timingEdgeValue(*now, dragEdge));
            }
        }else{
            dragEdge = poseui::TimingHit();
        }
    }
    ImGui::EndChild();
}

// ---- tabs ----

void editor::PoseAnimationWindow::drawTabs(){
    if (!ImGui::BeginTabBar("##PoseTabs")) return;
    auto tabFlags = [this](const char* name){
        if (requestedTab == name){
            requestedTab.clear();
            return ImGuiTabItemFlags_SetSelected;
        }
        return ImGuiTabItemFlags_None;
    };
    if (ImGui::BeginTabItem("Pose", nullptr, tabFlags("Pose"))){
        activeTab = "Pose";
        ImGui::BeginChild("##PoseTabBody");
        drawPoseTab();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Feel", nullptr, tabFlags("Feel"))){
        activeTab = "Feel";
        ImGui::BeginChild("##FeelTabBody");
        drawFeelTab();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Humanize", nullptr, tabFlags("Humanize"))){
        activeTab = "Humanize";
        ImGui::BeginChild("##HumanizeTabBody");
        drawHumanizeTab();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    for (const Tab& tab : extraTabs()){
        if (ImGui::BeginTabItem(tab.name.c_str(), nullptr, tabFlags(tab.name.c_str()))){
            activeTab = tab.name;
            ImGui::BeginChild((std::string("##TabBody") + tab.name).c_str());
            if (tab.draw) tab.draw(*this);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
    }
    ImGui::EndTabBar();
}

void editor::PoseAnimationWindow::drawCurvePlot(const pose::Transition& transition, const ImVec2& size){
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), color(Theme::Colors::ToolbarBackground), 3.0f);
    // 0 and 1 levels, the snap time at x = 1 (2/3 of the width; the settle after it)
    const float x1 = origin.x + size.x * 2.0f / 3.0f;
    const float y0 = origin.y + size.y * 0.8f;
    const float y1 = origin.y + size.y * 0.3f;
    draw->AddLine(ImVec2(origin.x, y0), ImVec2(origin.x + size.x, y0), color(Theme::Colors::SubtleText, 0.35f));
    draw->AddLine(ImVec2(origin.x, y1), ImVec2(origin.x + size.x, y1), color(Theme::Colors::SubtleText, 0.35f));
    draw->AddLine(ImVec2(x1, origin.y), ImVec2(x1, origin.y + size.y), color(Theme::Colors::SubtleText, 0.25f));
    const int samples = 72;
    const std::vector<float> values = poseui::easeSamples(transition, samples, 1.5f);
    std::vector<ImVec2> points;
    for (int i = 0; i < samples; i++){
        const float x = origin.x + size.x * static_cast<float>(i) / (samples - 1);
        const float y = y0 - (y0 - y1) * values[i];
        points.push_back(ImVec2(x, std::clamp(y, origin.y + 1.0f, origin.y + size.y - 1.0f)));
    }
    draw->AddPolyline(points.data(), samples, color(Theme::Colors::PrefabText), ImDrawFlags_None, Theme::dpi(1.5f));
}

void editor::PoseAnimationWindow::drawPoseTab(){
    // a snapshot: an edit committed below replaces the document's data, not this
    const std::shared_ptr<const pose::PoseAnimationData> held = view();
    const pose::PoseAnimationData* animation = held.get();
    if (!animation) return;
    const int primary = session.primary();
    if (primary < 0 || primary >= static_cast<int>(animation->poses.size())){
        caption("Select a pose in the strip.");
        return;
    }
    const std::vector<int> sel = selection();
    const pose::PoseKey& key = animation->poses[primary];
    const float labelWidth = Theme::dpi(110.0f);
    const bool multi = sel.size() > 1;
    if (multi){
        ImGui::TextColored(Theme::Colors::SubtleText, "%d poses selected: edits apply to all", static_cast<int>(sel.size()));
    }

    // name (single pose)
    fieldLabel("Name", labelWidth);
    char name[128];
    std::snprintf(name, sizeof(name), "%s", multi ? "" : key.name.c_str());
    ImGui::BeginDisabled(multi);
    if (ImGui::InputText("##PoseName", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()){
        if (key.name != name){
            pose::PoseAnimationData edited = *animation;
            edited.poses[primary].name = name;
            commit(edited, "Rename pose");
        }
    }
    ImGui::EndDisabled();

    // hold
    fieldLabel("Hold", labelWidth);
    poseui::MixedValue hold = poseui::mixedValue(*animation, sel, [](const pose::PoseKey& k){ return k.hold; });
    float holdValue = hold.value;
    if (mixedSlider("##PoseHold", holdValue, hold.mixed, pose::ranges::HOLD_MIN, pose::ranges::HOLD_MAX, "%.2f s", true, 0.005f)){
        pose::PoseAnimationData edited = *animation;
        poseui::setOnSelection(edited, sel, [holdValue](pose::PoseKey& k){ k.hold = holdValue; });
        commit(edited, "Hold time", widgetMergeKey("hold"));
    }
    finishWidgetEdit();
    ImGui::SetItemTooltip("Seconds the pose is held before its transition (drag the timing bar too)");

    // grounded / lift
    fieldLabel("Grounded", labelWidth);
    bool grounded = key.grounded;
    bool groundedMixed = false;
    for (int i : sel) groundedMixed |= animation->poses[i].grounded != key.grounded;
    ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, groundedMixed);
    if (ImGui::Checkbox("##PoseGrounded", &grounded)){
        pose::PoseAnimationData edited = *animation;
        poseui::setOnSelection(edited, sel, [grounded](pose::PoseKey& k){ k.grounded = grounded; if (grounded) k.lift = 0.0f; });
        commit(edited, "Grounded");
    }
    ImGui::PopItemFlag();
    ImGui::SetItemTooltip("Keep the lowest point on the floor (off: the pose floats at Lift)");
    if (!grounded || groundedMixed){
        fieldLabel("Lift", labelWidth);
        poseui::MixedValue lift = poseui::mixedValue(*animation, sel, [](const pose::PoseKey& k){ return k.lift; });
        float liftValue = lift.value;
        if (mixedSlider("##PoseLift", liftValue, lift.mixed, pose::ranges::LIFT_MIN, pose::ranges::LIFT_MAX, "%.3f", true, 0.005f)){
            pose::PoseAnimationData edited = *animation;
            poseui::setOnSelection(edited, sel, [liftValue](pose::PoseKey& k){ if (!k.grounded) k.lift = liftValue; });
            commit(edited, "Lift", widgetMergeKey("lift"));
        }
        finishWidgetEdit();
        ImGui::SetItemTooltip("Height above the floor (character heights x 0.95 / hips; about metres for a person)");
    }

    // offset
    fieldLabel("Offset", labelWidth);
    poseui::MixedValue offsetLeft = poseui::mixedValue(*animation, sel, [](const pose::PoseKey& k){ return k.body.offset.x; });
    poseui::MixedValue offsetForward = poseui::mixedValue(*animation, sel, [](const pose::PoseKey& k){ return k.body.offset.z; });
    const float half = (fieldWidth() - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    float leftValue = offsetLeft.value;
    ImGui::SetNextItemWidth(half);
    if (mixedSlider("##PoseOffsetLeft", leftValue, offsetLeft.mixed, pose::ranges::OFFSET_MIN, pose::ranges::OFFSET_MAX, "left %.3f", true, 0.002f)){
        pose::PoseAnimationData edited = *animation;
        poseui::setOnSelection(edited, sel, [leftValue](pose::PoseKey& k){ k.body.offset.x = leftValue; });
        commit(edited, "Offset", widgetMergeKey("offsetLeft"));
    }
    finishWidgetEdit();
    ImGui::SetItemTooltip("Hips offset towards the character's left (humanScale units)");
    ImGui::SameLine();
    float forwardValue = offsetForward.value;
    ImGui::SetNextItemWidth(half);
    if (mixedSlider("##PoseOffsetForward", forwardValue, offsetForward.mixed, pose::ranges::OFFSET_MIN, pose::ranges::OFFSET_MAX, "forward %.3f", true, 0.002f)){
        pose::PoseAnimationData edited = *animation;
        poseui::setOnSelection(edited, sel, [forwardValue](pose::PoseKey& k){ k.body.offset.z = forwardValue; });
        commit(edited, "Offset", widgetMergeKey("offsetForward"));
    }
    finishWidgetEdit();
    ImGui::SetItemTooltip("Hips offset forwards (humanScale units)");

    // custom transition
    ImGui::Spacing();
    if (focusTransitionFields){
        ImGui::SetScrollHereY(0.2f);
    }
    fieldLabel("Custom transition", labelWidth);
    bool custom = key.customTransition;
    bool customMixed = false;
    for (int i : sel) customMixed |= animation->poses[i].customTransition != key.customTransition;
    ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, customMixed);
    if (focusTransitionFields){
        ImGui::SetKeyboardFocusHere();
        focusTransitionFields = false;
    }
    if (ImGui::Checkbox("##PoseCustom", &custom)){
        pose::PoseAnimationData edited = *animation;
        const pose::Transition base = animation->settings.transition;
        poseui::setOnSelection(edited, sel, [custom, base](pose::PoseKey& k){
            if (custom && !k.customTransition) k.transition = base;
            k.customTransition = custom;
        });
        commit(edited, "Custom transition");
    }
    ImGui::PopItemFlag();
    ImGui::SetItemTooltip("This pose's own transition out (off: the Feel tab's)");
    if (custom){
        const pose::Transition& transition = key.transition;
        fieldLabel("Curve", labelWidth);
        if (ImGui::BeginCombo("##PoseCurve", curveLabel(transition.curve))){
            for (pose::TransitionCurve curve : {pose::TransitionCurve::Spring, pose::TransitionCurve::Linear, pose::TransitionCurve::Smooth}){
                if (ImGui::Selectable(curveLabel(curve), curve == transition.curve)){
                    pose::PoseAnimationData edited = *animation;
                    poseui::setOnSelection(edited, sel, [curve](pose::PoseKey& k){ if (k.customTransition) k.transition.curve = curve; });
                    commit(edited, "Transition curve");
                }
            }
            ImGui::EndCombo();
        }
        auto transitionSlider = [&](const char* label, const char* id, float pose::Transition::*field, float min, float max,
                                    const char* format, const char* tip, bool enabled){
            fieldLabel(label, labelWidth);
            poseui::MixedValue mixed = poseui::mixedValue(*animation, sel, [field](const pose::PoseKey& k){ return k.transition.*field; });
            float value = mixed.value;
            ImGui::BeginDisabled(!enabled);
            if (mixedSlider(id, value, mixed.mixed, min, max, format)){
                pose::PoseAnimationData edited = *animation;
                poseui::setOnSelection(edited, sel, [field, value](pose::PoseKey& k){ if (k.customTransition) k.transition.*field = value; });
                commit(edited, "Transition", widgetMergeKey(id));
            }
            finishWidgetEdit();
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("%s", tip);
        };
        const bool spring = transition.curve == pose::TransitionCurve::Spring;
        transitionSlider("Duration", "##PoseDuration", &pose::Transition::duration, pose::ranges::DURATION_MIN, pose::ranges::DURATION_MAX,
                         "%.2f s", "Snap time: when the next pose is first reached", true);
        transitionSlider("Overshoot", "##PoseOvershoot", &pose::Transition::overshoot, pose::ranges::OVERSHOOT_MIN, pose::ranges::OVERSHOOT_MAX,
                         "%.2f", "How far the spring swings past the pose (spring only)", spring);
        transitionSlider("Anticipation", "##PoseAnticipation", &pose::Transition::anticipation, pose::ranges::ANTICIPATION_MIN,
                         pose::ranges::ANTICIPATION_MAX, "%.2f", "Wind-up before the move (spring only)", spring);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelWidth);
        drawCurvePlot(transition, ImVec2(std::max(Theme::dpi(60.0f), fieldWidth()), Theme::dpi(54.0f)));
    }

    // tools
    ImGui::Spacing();
    if (ImGui::Button("Mirror")) actionMirror();
    ImGui::SetItemTooltip("Swap left and right (M)");
    ImGui::SameLine();
    if (ImGui::Button("Reset to Neutral")) actionResetNeutral();
    ImGui::SetItemTooltip("Every joint angle to 0: the neutral standing pose");
    ImGui::SameLine();
    if (ImGui::Button("Ground")) actionGround();
    ImGui::SetItemTooltip("Put the pose on the floor (G)");
    ImGui::SameLine();
    if (ImGui::Button("Center")) actionCenter();
    ImGui::SetItemTooltip("Hips offset back to 0");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_COPY "###PoseCopy")) actionCopy();
    ImGui::SetItemTooltip("Copy Pose (Ctrl/Cmd+C)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!clipboard.has_value());
    if (ImGui::Button(ICON_FA_PASTE "###PosePaste")) actionPaste();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Paste Pose: keeps the name, hold and transition (Ctrl/Cmd+V)");

    // the viewport's ghosts and markers (phase 5 reads them from the session)
    ImGui::Spacing();
    PoseEditSession::OnionSettings& onion = session.onion();
    PoseEditSession::ViewSettings& view = session.view();
    ImGui::TextColored(Theme::Colors::SubtleText, "Viewport");
    // the Scene view's posing (phase 5) and its keys
    ImGui::SetItemTooltip("Scene view while posing:\n"
                          "- drag a hand or foot sphere: the limb bends (IK); feet snap to the floor\n"
                          "- drag the hips cube: grounded poses keep their feet planted\n"
                          "- click a joint dot: rings to rotate it (red L, green U, blue F, white: about the view)\n"
                          "- Shift: hands and feet stay on the horizontal plane\n"
                          "- Ctrl/Cmd: rings turn in 5 degree steps\n"
                          "- E: Global Edit, Escape: cancel it\n"
                          "- click empty space or another object: stop posing");
    ImGui::Checkbox("Previous pose", &onion.previous);
    ImGui::SetItemTooltip("Onion skin: the previous pose as a blue ghost");
    ImGui::SameLine();
    ImGui::Checkbox("Next pose", &onion.next);
    ImGui::SetItemTooltip("Onion skin: the next pose as a green ghost");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(90.0f));
    ImGui::SliderFloat("##PoseOnionOpacity", &onion.opacity, 0.05f, 1.0f, "opacity %.2f");
    ImGui::SetItemTooltip("How visible the ghosts are (default 0.5)");
    ImGui::Checkbox("Show balance", &view.showBalance);
    ImGui::SetItemTooltip("The centre of mass on the floor: green when balanced over the feet");
    ImGui::SameLine();
    ImGui::Checkbox("Ghost during preview", &view.ghostDuringPreview);
    ImGui::SetItemTooltip("While previewing, ghost the key pose the playhead is leaving");

    drawJointAngles();
}

void editor::PoseAnimationWindow::drawJointAngles(){
    // a snapshot: an edit committed below replaces the document's data, not this
    const std::shared_ptr<const pose::PoseAnimationData> held = view();
    const pose::PoseAnimationData* animation = held.get();
    if (!animation) return;
    ImGui::Spacing();
    if (!Theme::foldoutHeader("Joint angles")) return;
    const std::vector<int> sel = selection();
    const std::shared_ptr<pose::PoseRig>& rig = session.rig();
    const float labelWidth = Theme::dpi(110.0f);
    for (const poseui::DofGroup& group : poseui::dofGroups()){
        if (!ImGui::TreeNodeEx(group.name, ImGuiTreeNodeFlags_SpanAvailWidth)) continue;
        for (pose::HumanBone bone : group.bones){
            const bool mapped = !rig || rig->has(bone);
            const std::string boneName = pose::humanBoneName(bone);
            ImGui::PushID(boneName.c_str());
            ImGui::AlignTextToFramePadding();
            if (mapped) ImGui::TextUnformatted(boneName.c_str());
            else ImGui::TextColored(Theme::Colors::SubtleText, "%s (not mapped)", boneName.c_str());
            for (int k = 0; k < pose::DOFS_PER_BONE; k++){
                const int dof = pose::dofIndex(bone, k);
                const pose::DofInfo& info = pose::dofInfo(dof);
                poseui::MixedValue value = poseui::mixedAngle(*animation, sel, dof);
                float degrees = value.value;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Theme::dpi(12.0f));
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(info.name);
                ImGui::SameLine(labelWidth);
                const bool hipsBone = bone == pose::HumanBone::Hips;
                const std::string range = hipsBone ? std::string("free") : fixed(info.minDegrees, 0) + ".." + fixed(info.maxDegrees, 0);
                const float rangeWidth = ImGui::CalcTextSize("-180..180").x + Theme::dpi(8.0f);
                ImGui::SetNextItemWidth(std::max(Theme::dpi(60.0f), ImGui::GetContentRegionAvail().x - rangeWidth));
                const bool outside = poseui::outsideLimits(dof, degrees);
                if (outside) ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
                const std::string id = "##" + std::to_string(k);
                if (ImGui::DragFloat(id.c_str(), &degrees, 0.25f, -180.0f, 180.0f, value.mixed ? "--" : "%.1f\xC2\xB0")){
                    pose::PoseAnimationData edited = *animation;
                    poseui::setAngle(edited, sel, dof, degrees);
                    if (edited.settings.humanize.jointLimits){
                        for (int i : sel){
                            if (i >= 0 && i < static_cast<int>(edited.poses.size())) pose::clampToLimits(edited.poses[i].body);
                        }
                    }
                    commit(edited, "Joint angle", widgetMergeKey(("angle" + std::to_string(dof)).c_str()));
                }
                finishWidgetEdit();
                if (outside) ImGui::PopStyleColor();
                ImGui::SetItemTooltip("%s.%s: degrees from the neutral standing pose (limits %s)%s", boneName.c_str(), info.name, range.c_str(),
                                      outside ? "\nOutside the joint limits" : "");
                ImGui::SameLine();
                ImGui::TextColored(Theme::Colors::SubtleText, "%s", range.c_str());
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
}

void editor::PoseAnimationWindow::drawFeelTab(){
    // a snapshot: an edit committed below replaces the document's data, not this
    const std::shared_ptr<const pose::PoseAnimationData> held = view();
    const pose::PoseAnimationData* animation = held.get();
    if (!animation) return;
    const pose::PoseAnimationSettings& settings = animation->settings;
    const float labelWidth = Theme::dpi(110.0f);

    // preset chips
    const int lit = poseui::matchingPresetIndex(settings);
    const std::vector<pose::FeelPreset>& presets = pose::feelPresets();
    for (size_t i = 0; i < presets.size(); i++){
        if (i > 0){
            const float next = ImGui::CalcTextSize(presets[i].name).x + ImGui::GetStyle().FramePadding.x * 2;
            const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
            if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + next < right) ImGui::SameLine();
        }
        const bool on = static_cast<int>(i) == lit;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::Selection);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight() * 0.5f);
        if (ImGui::Button(presets[i].name)){
            pose::PoseAnimationData edited = *animation;
            pose::applyFeelPreset(presets[i], edited.settings);
            commit(edited, "Feel preset");
        }
        ImGui::PopStyleVar();
        if (on) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s: %s %.2f s, overshoot %.2f, wind-up %.2f, overlap %.2f s, looseness %.2f%s", presets[i].name,
                              curveLabel(presets[i].transition.curve), presets[i].transition.duration, presets[i].transition.overshoot,
                              presets[i].transition.anticipation, presets[i].overlap, presets[i].looseness,
                              presets[i].stepRate > 0 ? (", choppy at " + fixed(presets[i].stepRate, 0)).c_str() : "");
    }
    caption(lit >= 0 ? "The lit preset matches the settings below." : "Custom feel (no preset matches).");
    ImGui::Spacing();

    auto settingsSlider = [&](const char* label, const char* id, float value, float min, float max, const char* format,
                              const std::function<void(pose::PoseAnimationSettings&, float)>& set, const char* help, bool enabled = true){
        fieldLabel(label, labelWidth);
        ImGui::BeginDisabled(!enabled);
        if (ImGui::SliderFloat(id, &value, min, max, format, ImGuiSliderFlags_AlwaysClamp)){
            pose::PoseAnimationData edited = *animation;
            set(edited.settings, value);
            commit(edited, label, widgetMergeKey(id));
        }
        finishWidgetEdit();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", help);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelWidth);
        caption(help);
    };

    fieldLabel("Curve", labelWidth);
    if (ImGui::BeginCombo("##FeelCurve", curveLabel(settings.transition.curve))){
        for (pose::TransitionCurve curve : {pose::TransitionCurve::Spring, pose::TransitionCurve::Linear, pose::TransitionCurve::Smooth}){
            if (ImGui::Selectable(curveLabel(curve), curve == settings.transition.curve)){
                pose::PoseAnimationData edited = *animation;
                edited.settings.transition.curve = curve;
                commit(edited, "Feel curve");
            }
        }
        ImGui::EndCombo();
    }
    const bool spring = settings.transition.curve == pose::TransitionCurve::Spring;
    settingsSlider("Snap time", "##FeelDuration", settings.transition.duration, pose::ranges::DURATION_MIN, pose::ranges::DURATION_MAX, "%.2f s",
                   [](pose::PoseAnimationSettings& s, float v){ s.transition.duration = v; }, "Seconds from one pose to the next");
    settingsSlider("Overshoot", "##FeelOvershoot", settings.transition.overshoot, pose::ranges::OVERSHOOT_MIN, pose::ranges::OVERSHOOT_MAX, "%.2f",
                   [](pose::PoseAnimationSettings& s, float v){ s.transition.overshoot = v; }, "How far the spring swings past each pose", spring);
    settingsSlider("Wind-up", "##FeelAnticipation", settings.transition.anticipation, pose::ranges::ANTICIPATION_MIN,
                   pose::ranges::ANTICIPATION_MAX, "%.2f", [](pose::PoseAnimationSettings& s, float v){ s.transition.anticipation = v; },
                   "A small move backwards before each move", spring);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelWidth);
    drawCurvePlot(settings.transition, ImVec2(std::max(Theme::dpi(60.0f), fieldWidth()), Theme::dpi(60.0f)));
    ImGui::Spacing();
    settingsSlider("Choppiness", "##FeelStepRate", settings.stepRate, pose::ranges::STEP_RATE_MIN, pose::ranges::STEP_RATE_MAX,
                   settings.stepRate <= 0.0f ? "off" : (std::abs(settings.stepRate - 12.0f) < 0.01f ? "%.0f steps/s (on twos)" : "%.0f steps/s"),
                   [](pose::PoseAnimationSettings& s, float v){ s.stepRate = std::round(v); },
                   "Held frames like stop motion: steps per second, 0 = smooth (12 = \"on twos\")");
    settingsSlider("Overlap", "##FeelOverlap", settings.overlap, pose::ranges::OVERLAP_MIN, pose::ranges::OVERLAP_MAX, "%.2f s",
                   [](pose::PoseAnimationSettings& s, float v){ s.overlap = v; }, "Hands and feet arrive later than the body");
    settingsSlider("Looseness", "##FeelLooseness", settings.looseness, pose::ranges::LOOSENESS_MIN, pose::ranges::LOOSENESS_MAX, "%.2f",
                   [](pose::PoseAnimationSettings& s, float v){ s.looseness = v; }, "Trailing parts take longer and swing further");
    ImGui::Spacing();
    ImGui::TextColored(Theme::Colors::SubtleText, "Body weights (how much overlap and looseness each part gets)");
    static const char* groups[] = {"Spine", "Head", "Arms", "Legs"};
    static const char* help[] = {"Spine: 0 = moves as one block", "Head: lags behind the chest", "Arms: drag behind the shoulders",
                                 "Legs: usually 0 so feet stay planted"};
    for (int g = 0; g < 4; g++){
        const std::string id = std::string("##FeelWeight") + groups[g];
        settingsSlider(groups[g], id.c_str(), settings.weights[g], pose::ranges::WEIGHT_MIN, pose::ranges::WEIGHT_MAX, "%.2f",
                       [g](pose::PoseAnimationSettings& s, float v){ s.weights[g] = v; }, help[g]);
    }
}

void editor::PoseAnimationWindow::drawHumanizeTab(){
    // a snapshot: an edit committed below replaces the document's data, not this
    const std::shared_ptr<const pose::PoseAnimationData> held = view();
    const pose::PoseAnimationData* animation = held.get();
    if (!animation) return;
    const pose::HumanizeSettings& humanize = animation->settings.humanize;
    const float labelWidth = Theme::dpi(110.0f);
    auto toggle = [&](const char* label, const char* id, bool value, const std::function<void(pose::HumanizeSettings&, bool)>& set, const char* help){
        fieldLabel(label, labelWidth);
        if (ImGui::Checkbox(id, &value)){
            pose::PoseAnimationData edited = *animation;
            set(edited.settings.humanize, value);
            commit(edited, label);
        }
        ImGui::SetItemTooltip("%s", help);
        ImGui::SameLine();
        caption(help);
    };
    auto slider = [&](const char* label, const char* id, float value, float min, float max, const char* format,
                      const std::function<void(pose::HumanizeSettings&, float)>& set, const char* help, bool enabled){
        fieldLabel(label, labelWidth);
        ImGui::BeginDisabled(!enabled);
        if (ImGui::SliderFloat(id, &value, min, max, format, ImGuiSliderFlags_AlwaysClamp)){
            pose::PoseAnimationData edited = *animation;
            set(edited.settings.humanize, value);
            commit(edited, label, widgetMergeKey(id));
        }
        finishWidgetEdit();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", help);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelWidth);
        caption(help);
    };
    toggle("Humanize", "##HumanizeEnabled", humanize.enabled, [](pose::HumanizeSettings& h, bool v){ h.enabled = v; },
           "Secondary motion that makes poses feel alive");
    toggle("Joint limits", "##HumanizeLimits", humanize.jointLimits, [](pose::HumanizeSettings& h, bool v){ h.jointLimits = v; },
           "Keep every joint in a human range (applies even with Humanize off)");
    const bool on = humanize.enabled;
    slider("Moving holds", "##HumanizeHolds", humanize.movingHolds, pose::ranges::MOVING_HOLDS_MIN, pose::ranges::MOVING_HOLDS_MAX, "%.2f",
           [](pose::HumanizeSettings& h, float v){ h.movingHolds = v; }, "Drift towards the next pose during holds", on);
    slider("In-betweens", "##HumanizeInbetweens", humanize.inbetweens, pose::ranges::INBETWEENS_MIN, pose::ranges::INBETWEENS_MAX, "%.2f",
           [](pose::HumanizeSettings& h, float v){ h.inbetweens = v; }, "Limits how fast a joint turns per frame: softer snaps", on);
    slider("Life", "##HumanizeLife", humanize.life, pose::ranges::LIFE_MIN, pose::ranges::LIFE_MAX, "%.1f\xC2\xB0",
           [](pose::HumanizeSettings& h, float v){ h.life = v; }, "A gentle sway so the character never freezes (degrees)", on);
    fieldLabel("Seed", labelWidth);
    int seed = static_cast<int>(std::min<uint32_t>(humanize.seed, pose::ranges::SEED_MAX));
    ImGui::BeginDisabled(!on);
    ImGui::SetNextItemWidth(std::max(Theme::dpi(60.0f), fieldWidth() - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x));
    if (ImGui::InputInt("##HumanizeSeed", &seed, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue) && seed >= 0){
        pose::PoseAnimationData edited = *animation;
        edited.settings.humanize.seed = static_cast<uint32_t>(seed);
        commit(edited, "Seed");
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_DICE "###HumanizeDice", ImVec2(ImGui::GetFrameHeight(), 0))){
        static std::mt19937 random(std::random_device{}());
        pose::PoseAnimationData edited = *animation;
        edited.settings.humanize.seed = std::uniform_int_distribution<uint32_t>(0, pose::ranges::SEED_MAX)(random);
        commit(edited, "Seed");
    }
    ImGui::SetItemTooltip("A new random seed");
    ImGui::EndDisabled();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelWidth);
    caption("Picks the sway pattern: same seed, same motion");
    slider("Head follow", "##HumanizeHead", humanize.headFollow, pose::ranges::FOLLOW_MIN, pose::ranges::FOLLOW_MAX, "%.2f",
           [](pose::HumanizeSettings& h, float v){ h.headFollow = v; }, "The head tilts as the body speeds up and lands", on);
    slider("Torso follow", "##HumanizeTorso", humanize.torsoFollow, pose::ranges::FOLLOW_MIN, pose::ranges::FOLLOW_MAX, "%.2f",
           [](pose::HumanizeSettings& h, float v){ h.torsoFollow = v; }, "The torso leans against the hips' acceleration", on);
    slider("Springiness", "##HumanizeSpring", humanize.springiness, pose::ranges::SPRINGINESS_MIN, pose::ranges::SPRINGINESS_MAX, "%.2f",
           [](pose::HumanizeSettings& h, float v){ h.springiness = v; }, "0 = damped follow-through, 1 = bouncy", on);
}

// ---- humanoid mapping popup ----

void editor::PoseAnimationWindow::openMappingPopup(uint32_t sceneId, Entity entity){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || !sceneProject->scene->isEntityCreated(entity)) return;
    mappingScene = sceneId;
    mappingEntity = entity;
    mappingInfo = PoseRigProvider::forSceneModel(project, sceneProject->scene, entity);
    mappingOverrides = {};
    mappingSidecar = pose::HumanoidOverrides();
    mappingError.clear();
    mappingFilter[0] = '\0';
    // the sidecar's overrides as they are
    if (ModelComponent* model = sceneProject->scene->findComponent<ModelComponent>(entity)){
        const fs::path sidecar = project->resolveAssetPath(editor::FileUtils::pathFromUtf8(pose::humanoidSidecarPath(model->filename)));
        std::string text;
        std::error_code ec;
        if (fs::exists(sidecar, ec) && ProjectChecks::readFile(sidecar, text)){
            pose::HumanoidOverrides overrides;
            std::vector<std::string> warnings;
            if (pose::parseHumanoidOverrides(text, overrides, warnings, nullptr)){
                mappingOverrides = overrides.bones;
                mappingSidecar = overrides;
            }
        }
    }
    mappingRequested = true;
}

void editor::PoseAnimationWindow::applyMapping(){
    SceneProject* sceneProject = project->getScene(mappingScene);
    if (!sceneProject || !sceneProject->scene) return;
    ModelComponent* model = sceneProject->scene->findComponent<ModelComponent>(mappingEntity);
    if (!model) return;
    if (mappingInfo && !mappingInfo->sidecarError.empty()){
        // the bones are written into the sidecar as read: an unreadable one is not rewritten
        mappingError = PoseRigProvider::unreadableSidecarRefusal(mappingInfo->sidecarError);
        return;
    }
    const fs::path sidecar = project->resolveAssetPath(editor::FileUtils::pathFromUtf8(pose::humanoidSidecarPath(model->filename)));
    // the bones only: the Avatar's Animation Type, extra bones and twists stay
    pose::HumanoidOverrides overrides = mappingSidecar;
    overrides.bones = mappingOverrides;
    const std::string text = pose::writeHumanoidOverrides(overrides);
    std::string error;
    const auto& doc = session.document();
    bool written = false;
    if (doc && doc->exists()){
        // the set_humanoid path: the sidecar in the document's undo step
        written = doc->commit(doc->data(), "Map bones", "", nullptr, &error, {{sidecar, text}});
    }else{
        auto* command = new WriteFilesCmd({{sidecar, text}}, [](const std::vector<fs::path>& paths){
            PoseAnimationDocument::filesChanged(paths);
        });
        command->setNoMerge();
        written = project->getProjectCommandHistory()->addCommand(command);
        if (!written) error = "writing " + sidecar.generic_string() + " failed";
    }
    if (!written){
        mappingError = error;
        return;
    }
    // the rig follows the new mapping
    std::string bindError;
    if (bindCharacter(mappingScene, mappingEntity, &bindError)){
        if (data() && !data()->poses.empty()) session.startPosing();
        setStatus("Bone mapping saved", false);
        mappingOpen = false;
        ImGui::CloseCurrentPopup();
    }else{
        mappingError = bindError;
        mappingInfo = PoseRigProvider::forSceneModel(project, sceneProject->scene, mappingEntity);
    }
}

void editor::PoseAnimationWindow::drawMappingPopup(){
    if (mappingRequested){
        mappingRequested = false;
        mappingOpen = true;
    }
    // open until Apply, Cancel or its close button (a focus change elsewhere closes ImGui popups:
    // it opens again)
    if (mappingOpen && !ImGui::IsPopupOpen("Map Bones###PoseMapBones")){
        ImGui::OpenPopup("Map Bones###PoseMapBones");
    }
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(560.0f), Theme::dpi(560.0f)), ImGuiCond_Appearing);
    bool open = mappingOpen;
    if (!EditorModal::begin("Map Bones###PoseMapBones", &open)){
        if (!open) mappingOpen = false;
        return;
    }
    if (!mappingInfo){
        ImGui::TextUnformatted("The model is gone.");
        EditorModal::message("The model is gone.");
        if (EditorModal::button("Close", "ok")){
            mappingOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }
    EditorModal::message("Pick the skeleton bone of each humanoid slot (a field per slot; the empty text: none). Apply writes " +
                         editor::FileUtils::pathToUtf8(editor::FileUtils::pathFromUtf8(mappingInfo->model).filename()) + ".humanoid (one undo step).");
    ImGui::TextWrapped("Pick the skeleton bone of each humanoid slot. Slots marked * are required; \"Auto\" uses the name-based detection. "
                       "Apply writes %s.humanoid (one undo step).", editor::FileUtils::pathToUtf8(editor::FileUtils::pathFromUtf8(mappingInfo->model).filename()).c_str());
    ImGui::SetNextItemWidth(Theme::dpi(220.0f));
    ImGui::InputTextWithHint("##MapFilter", ICON_FA_MAGNIFYING_GLASS " filter bones", mappingFilter, sizeof(mappingFilter));
    const std::string filter = [&]{
        std::string f = mappingFilter;
        for (char& c : f) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return f;
    }();
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2 + Theme::dpi(8.0f);
    if (ImGui::BeginTable("##MapTable", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH,
                          ImVec2(0, ImGui::GetContentRegionAvail().y - footer))){
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(150.0f));
        ImGui::TableSetupColumn("Bone", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(120.0f));
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        static const char* groupNames[] = {"Spine", "Head", "Arms", "Legs"};
        for (int g = 0; g < static_cast<int>(pose::BodyGroup::Count); g++){
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(Theme::Colors::SubtleText, "%s", groupNames[g]);
            for (int b = 0; b < pose::HUMAN_BONE_COUNT; b++){
                const pose::HumanBone bone = pose::boneAt(b);
                const pose::HumanBoneInfo& info = pose::humanBoneInfo(bone);
                if (static_cast<int>(info.group) != g) continue;
                ImGui::PushID(b);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s%s", info.name, info.required ? " *" : "");
                ImGui::TableSetColumnIndex(1);
                const std::optional<std::string>& override = mappingOverrides[b];
                std::string agentBone = override ? *override : mappingInfo->humanoid.bones[b];
                if (EditorModal::field(info.name, agentBone)) mappingOverrides[b] = agentBone;
                const std::string current = override ? *override : mappingInfo->humanoid.bones[b];
                const std::string preview = current.empty() ? std::string("(none)") : current;
                ImGui::SetNextItemWidth(-Theme::dpi(50.0f));
                if (ImGui::BeginCombo("##bone", preview.c_str(), ImGuiComboFlags_HeightLarge)){
                    if (ImGui::Selectable("(none)", override && override->empty())) mappingOverrides[b] = std::string();
                    for (const std::string& name : mappingInfo->boneNames){
                        std::string lower = name;
                        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (!filter.empty() && lower.find(filter) == std::string::npos) continue;
                        if (ImGui::Selectable(name.c_str(), name == current)) mappingOverrides[b] = name;
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(!override.has_value());
                if (ImGui::SmallButton("Auto")) mappingOverrides[b].reset();
                ImGui::EndDisabled();
                ImGui::SetItemTooltip("Back to the name-based detection");
                ImGui::TableSetColumnIndex(2);
                ImGui::AlignTextToFramePadding();
                if (override && !override->empty()){
                    ImGui::TextColored(Theme::Colors::PrefabText, "override");
                }else if (current.empty() || (override && override->empty())){
                    if (info.required) ImGui::TextColored(Theme::Colors::ErrorText, "missing");
                    else ImGui::TextColored(Theme::Colors::SubtleText, override ? "unmapped" : "not found");
                }else{
                    ImGui::TextColored(Theme::Colors::SubtleText, "auto");
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    const bool unreadable = !mappingInfo->sidecarError.empty();
    EditorModal::error(!mappingError.empty() ? mappingError : unreadable ? PoseRigProvider::unreadableSidecarRefusal(mappingInfo->sidecarError) : std::string());
    if (!mappingError.empty()){
        ImGui::TextColored(Theme::Colors::ErrorText, "%s", mappingError.c_str());
    }else if (unreadable){
        ImGui::TextColored(Theme::Colors::ErrorText, "%s", PoseRigProvider::unreadableSidecarRefusal(mappingInfo->sidecarError).c_str());
    }else if (!mappingInfo->error.empty()){
        ImGui::TextColored(Theme::Colors::WarningText, "%s", mappingInfo->error.c_str());
    }else{
        ImGui::NewLine();
    }
    ImGui::BeginDisabled(unreadable);
    if (EditorModal::button("Apply", "ok")) applyMapping();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (EditorModal::button("Cancel", "cancel")){
        mappingOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    if (!open) mappingOpen = false;
}

// ---- open popup ----

void editor::PoseAnimationWindow::collectOpenCandidates(){
    openCandidates.clear();
    std::error_code ec;
    const fs::path root = project->getProjectPath();
    const fs::path assets = project->getAssetsPath();
    if (!fs::is_directory(assets, ec)) return;
    for (fs::recursive_directory_iterator it(assets, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)){
        if (ec) break;
        if (it->is_regular_file(ec) && Util::isPoseAnimationFile(it->path().string())){
            openCandidates.push_back(ProjectChecks::projectRelative(root, it->path()));
        }
        if (openCandidates.size() > 2000) break;
    }
    std::sort(openCandidates.begin(), openCandidates.end());
}

void editor::PoseAnimationWindow::drawOpenPopup(){
    if (openPopupRequested){
        openPopupRequested = false;
        collectOpenCandidates();
        openFilter[0] = '\0';
        ImGui::OpenPopup("##PoseOpenPopup");
    }
    if (!ImGui::BeginPopup("##PoseOpenPopup")) return;
    ImGui::SetNextItemWidth(Theme::dpi(280.0f));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##PoseOpenFilter", ICON_FA_MAGNIFYING_GLASS " search", openFilter, sizeof(openFilter));
    std::string filter = openFilter;
    for (char& c : filter) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    ImGui::BeginChild("##PoseOpenList", ImVec2(Theme::dpi(280.0f), Theme::dpi(220.0f)));
    int shown = 0;
    for (const std::string& path : openCandidates){
        std::string lower = path;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!filter.empty() && lower.find(filter) == std::string::npos) continue;
        shown++;
        if (ImGui::Selectable(path.c_str())){
            std::string error;
            if (!openAnimation(path, &error)) setStatus(error, true);
            ImGui::CloseCurrentPopup();
        }
    }
    if (shown == 0){
        ImGui::TextColored(Theme::Colors::SubtleText, openCandidates.empty() ? "No .poseanim files in the project" : "No match");
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}
