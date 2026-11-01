// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimatorWindow.h"

#include "Project.h"
#include "Scene.h"
#include "Theme.h"
#include "animation/AnimationClip.h"
#include "animation/ModelSubClips.h"
#include "component/Transform.h"
#include "external/IconsFontAwesome6.h"
#include "imgui_internal.h"
#include "pool/AnimationClipPool.h"
#include "subsystem/ActionSystem.h"
#include "util/ProjectChecks.h"
#include "util/UIProbe.h"
#include "util/Util.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    const ImU32 STATE_COLOR = IM_COL32(78, 78, 82, 255);
    const ImU32 DEFAULT_STATE_COLOR = IM_COL32(196, 118, 44, 255);
    const ImU32 ANY_STATE_COLOR = IM_COL32(38, 132, 128, 255);
    const ImU32 SELECTED_OUTLINE = IM_COL32(90, 170, 255, 255);
    const ImU32 ARROW_COLOR = IM_COL32(205, 205, 205, 255);
    const ImU32 ARROW_SELECTED = IM_COL32(90, 170, 255, 255);
    const ImU32 LIVE_COLOR = IM_COL32(60, 150, 255, 255);
    const float MIN_ZOOM = 0.2f;
    const float MAX_ZOOM = 2.5f;

    // a state's motion length in seconds (a clip's; a blend tree's first child; 1 when empty or
    // not loadable)
    float motionLength(const anim::ControllerState& state){
        std::string clip = state.clip;
        if (clip.empty() && state.hasBlendTree && !state.blendTree.children.empty()) clip = state.blendTree.children.front().clip;
        if (clip.empty()) return 1.0f;
        std::shared_ptr<const AnimationClip> loaded = AnimationClipPool::getOrLoad(clip);
        return loaded && loaded->getLength() > 0.0f ? loaded->getLength() : 1.0f;
    }

    std::string clipDisplayName(const std::string& clip){
        anim::SubClipReference reference;
        if (anim::parseSubClipReference(clip, reference)) return reference.clip;
        const fs::path path = fs::u8path(clip);
        return path.stem().u8string();
    }

    bool parameterIs(const anim::ControllerData& data, const std::string& name, std::initializer_list<anim::ControllerParameterType> types){
        const int index = data.findParameter(name);
        if (index < 0) return false;
        for (anim::ControllerParameterType type : types){
            if (data.parameters[(size_t)index].type == type) return true;
        }
        return false;
    }

    // a combo of the controller's parameters of some types (or none, with `allowNone`)
    bool parameterCombo(const char* label, const anim::ControllerData& data, std::string& value,
                        std::initializer_list<anim::ControllerParameterType> types, bool allowNone){
        bool changed = false;
        const std::string preview = value.empty() ? std::string("None") : value;
        if (ImGui::BeginCombo(label, preview.c_str())){
            if (allowNone && ImGui::Selectable("None", value.empty())){
                value.clear();
                changed = true;
            }
            for (const anim::ControllerParameter& parameter : data.parameters){
                bool fits = false;
                for (anim::ControllerParameterType type : types) fits = fits || parameter.type == type;
                if (!fits) continue;
                if (ImGui::Selectable(parameter.name.c_str(), parameter.name == value)){
                    value = parameter.name;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    void drawArrowHead(ImDrawList* drawList, ImVec2 at, ImVec2 direction, float size, ImU32 color){
        const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
        if (length <= 0.0f) return;
        const ImVec2 d(direction.x / length, direction.y / length);
        const ImVec2 n(-d.y, d.x);
        const ImVec2 tip(at.x + d.x * size, at.y + d.y * size);
        const ImVec2 left(at.x - d.x * size + n.x * size, at.y - d.y * size + n.y * size);
        const ImVec2 right(at.x - d.x * size - n.x * size, at.y - d.y * size - n.y * size);
        drawList->AddTriangleFilled(tip, left, right, color);
    }

}

std::vector<ClipWarning> editor::validateControllerClips(const anim::ControllerData& data){
    std::map<std::string, anim::ControllerClipInfo> cache;
    return anim::validateController(data, [&cache](const std::string& clip){
        auto it = cache.find(clip);
        if (it != cache.end()) return it->second;
        anim::ControllerClipInfo info;
        std::string error;
        std::shared_ptr<const AnimationClip> loaded = AnimationClipPool::getOrLoad(clip, &error);
        info.loads = loaded != nullptr;
        info.error = error;
        info.loops = loaded && (loaded->getWrap() == ClipWrapMode::Loop || loaded->getWrap() == ClipWrapMode::PingPong);
        cache[clip] = info;
        return info;
    });
}

editor::AnimatorWindow::AnimatorWindow(Project* project): project(project){
}

editor::AnimatorWindow::~AnimatorWindow(){
    close();
}

void editor::AnimatorWindow::setOpen(bool open){
    windowOpen = open;
    if (!open) stopPreview();
}

void editor::AnimatorWindow::setNotice(const std::string& text, bool isError){
    noticeText = text;
    noticeError = isError;
}

void editor::AnimatorWindow::close(){
    stopPreview();
    if (doc && listener) doc->removeListener(listener);
    listener = 0;
    doc.reset();
}

void editor::AnimatorWindow::attach(const std::shared_ptr<AnimatorControllerDocument>& document){
    if (doc == document) return;
    if (doc && listener) doc->removeListener(listener);
    listener = 0;
    doc = document;
    validationRevision = ~uint64_t(0);
    clearSelection();
    layer = 0;
    layerNames.clear();
    currentLayerName.clear();
    framePending = true;
    syncLayers();
    if (doc){
        listener = doc->addListener([this](AnimatorControllerDocument&, AnimatorControllerDocument::Change change){
            // undo, redo, an outside edit: indices may have moved
            validationRevision = ~uint64_t(0);
            if (change == AnimatorControllerDocument::Change::Removed){
                setNotice("The controller file is gone (an undo of its creation, or a delete)", true);
            }
            syncLayers();
        });
    }
}

bool editor::AnimatorWindow::openController(const std::string& controller, uint32_t scene, Entity target, std::string* errorOut){
    stopPreview();
    std::string openError;
    std::shared_ptr<AnimatorControllerDocument> document = AnimatorControllerDocument::open(project, controller, &openError);
    sceneId = scene;
    entity = target;
    {
        AnimationPlayerComponent* player = boundPlayer();
        boundControllerField = player ? player->controller : std::string();
    }
    if (!document){
        close();
        error = openError.empty() ? "cannot open " + controller : openError;
        if (errorOut) *errorOut = error;
        return false;
    }
    error.clear();
    attach(document);
    return true;
}

void editor::AnimatorWindow::followSelection(){
    SceneProject* sceneProject = project ? project->getSelectedScene() : nullptr;
    if (!sceneProject || !sceneProject->scene) return;
    const std::vector<Entity> selection = project->getSelectedEntities(sceneProject->id);
    if (sceneProject->id == followedScene && selection == followedSelection) return;
    followedScene = sceneProject->id;
    followedSelection = selection;
    if (selection.empty()) return;
    Scene* scene = sceneProject->scene;
    // the selected GameObject's player, or its nearest ancestor's
    Entity current = selection.front();
    for (int depth = 0; depth < 1024 && current != NULL_ENTITY && scene->isEntityCreated(current); depth++){
        if (AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(current)){
            if (!player->controller.empty()){
                if (sceneProject->id != sceneId || current != entity || !doc || error.size()){
                    openController(player->controller, sceneProject->id, current, nullptr);
                }
                return;
            }
            break;
        }
        const Transform* transform = scene->findComponent<Transform>(current);
        current = transform ? transform->parent : NULL_ENTITY;
    }
}

void editor::AnimatorWindow::stopTransientPreviews(){
    stopPreview();
}

void editor::AnimatorWindow::clearSceneState(uint32_t scene){
    if (scene != sceneId) return;
    stopPreview();
    sceneId = 0;
    entity = NULL_ENTITY;
}

void editor::AnimatorWindow::projectClosed(){
    close();
    error.clear();
    sceneId = 0;
    entity = NULL_ENTITY;
    followedScene = 0;
    followedSelection.clear();
    clipboard = animator::Clipboard();
    pickerStale = true;
}

void editor::AnimatorWindow::setCurrentLayer(int index){
    const int count = doc ? (int)doc->data().layers.size() : 0;
    const int clamped = count == 0 ? 0 : std::min(std::max(index, 0), count - 1);
    if (clamped != layer){
        clearSelection();
        framePending = true;
    }
    layer = clamped;
    currentLayerName = doc && (size_t)layer < doc->data().layers.size() ? doc->data().layers[(size_t)layer].name : std::string();
    // a selection past the data (after an undo) goes
    if (const anim::ControllerLayer* data = currentLayerData()){
        if (selected.kind == animator::ItemKind::State && (size_t)selected.index >= data->states.size()) clearSelection();
        if (selected.kind == animator::ItemKind::Transition && (size_t)selected.index >= data->transitions.size()) clearSelection();
        selectedStateList.erase(std::remove_if(selectedStateList.begin(), selectedStateList.end(),
                                               [&](int s){ return s < 0 || (size_t)s >= data->states.size(); }), selectedStateList.end());
    }
    if (doc && selected.kind == animator::ItemKind::Parameter && (size_t)selected.index >= doc->data().parameters.size()) clearSelection();
}

void editor::AnimatorWindow::syncLayers(){
    std::vector<std::string> names;
    if (doc){
        for (const anim::ControllerLayer& data : doc->data().layers) names.push_back(data.name);
    }
    if (names == layerNames){
        setCurrentLayer(layer);
        return;
    }
    // the layer list changed (an edit, an undo or redo, an outside change): the current layer
    // stays by name; a rename keeps its index; anything else falls back to the base layer
    int target = 0;
    auto found = std::find(names.begin(), names.end(), currentLayerName);
    if (found != names.end()) target = (int)(found - names.begin());
    else if (names.size() == layerNames.size() && (size_t)layer < names.size()) target = layer;
    const bool sameIndices = names.size() == layerNames.size() && target == layer;
    layerNames = names;
    // indices into the old list (states, transitions of another layer) must not survive
    if (!sameIndices) clearSelection();
    if (target != layer) framePending = true;
    layer = target;
    setCurrentLayer(target);
}

void editor::AnimatorWindow::followBoundController(){
    AnimationPlayerComponent* player = boundPlayer();
    if (entity == NULL_ENTITY || sceneId == 0) return;
    const std::string field = player ? player->controller : std::string();
    if (field == boundControllerField) return;
    boundControllerField = field;
    if (field.empty()){
        // the player lost its controller (the Inspector, MCP, an undo): nothing to show
        stopPreview();
        close();
        SceneProject* sceneProject = boundSceneProject();
        const std::string who = sceneProject && sceneProject->scene->isEntityCreated(entity) ? sceneProject->scene->getEntityName(entity) : std::string("the GameObject");
        error = who + "'s Animation Player has no Animator Controller";
        return;
    }
    openController(field, sceneId, entity, nullptr);
}

const anim::ControllerLayer* editor::AnimatorWindow::currentLayerData() const{
    if (!doc || layer < 0 || (size_t)layer >= doc->data().layers.size()) return nullptr;
    return &doc->data().layers[(size_t)layer];
}

SceneProject* editor::AnimatorWindow::boundSceneProject() const{
    if (!project || sceneId == 0) return nullptr;
    SceneProject* sceneProject = project->getScene(sceneId);
    return sceneProject && sceneProject->scene ? sceneProject : nullptr;
}

AnimationPlayerComponent* editor::AnimatorWindow::boundPlayer() const{
    SceneProject* sceneProject = boundSceneProject();
    if (!sceneProject || entity == NULL_ENTITY || !sceneProject->scene->isEntityCreated(entity)) return nullptr;
    return sceneProject->scene->findComponent<AnimationPlayerComponent>(entity);
}

std::string editor::AnimatorWindow::clipGuidOf(const std::string& clip){
    // an .anim's GUID (registered now when the file is new to the asset database); model clips
    // are path-only
    if (!project || clip.empty() || anim::isSubClipReference(clip)) return std::string();
    std::string path = clip;
    std::string guid;
    project->resolveAssetReference(path, guid, true);
    return guid;
}

int editor::AnimatorWindow::setBlendThreshold(int state, int child, float threshold){
    const anim::ControllerLayer* data = currentLayerData();
    if (!data || state < 0 || (size_t)state >= data->states.size() || !data->states[(size_t)state].hasBlendTree) return -1;
    const std::vector<anim::BlendTreeChild>& children = data->states[(size_t)state].blendTree.children;
    if (child < 0 || (size_t)child >= children.size()) return -1;
    // where the child lands: the normalize's stable sort by threshold
    std::vector<std::pair<float, int>> order;
    for (size_t c = 0; c < children.size(); c++) order.push_back({(int)c == child ? threshold : children[c].threshold, (int)c});
    std::stable_sort(order.begin(), order.end(), [](const std::pair<float, int>& a, const std::pair<float, int>& b){ return a.first < b.first; });
    int landed = child;
    for (size_t i = 0; i < order.size(); i++){
        if (order[i].second == child) landed = (int)i;
    }
    if (!editState(state, [child, threshold](anim::ControllerState& target){ target.blendTree.children[(size_t)child].threshold = threshold; })) return -1;
    return landed;
}

bool editor::AnimatorWindow::resolveTypedAsset(const std::string& typed, bool clip, std::string& assetRelative, std::string& guid){
    assetRelative.clear();
    guid.clear();
    if (typed.empty() || !project) return true;     // cleared
    // a model clip "<model>#<clip>": the model part is the file
    anim::SubClipReference reference;
    const bool subClip = clip && anim::parseSubClipReference(typed, reference);
    const std::string file = subClip ? reference.model : typed;
    // asset-relative, project-relative ("assets/..."), or absolute
    const fs::path given = fs::u8path(file);
    fs::path absolute;
    std::error_code ec;
    if (given.is_absolute()) absolute = given;
    else if (fs::is_regular_file(project->getAssetsPath() / given, ec)) absolute = project->getAssetsPath() / given;
    else absolute = project->getProjectPath() / given;
    const std::string relative = project->normalizeToAssetsRelative(absolute).generic_string();
    const bool inside = !relative.empty() && relative.rfind("..", 0) != 0 && !fs::u8path(relative).is_absolute();
    const char* what = clip ? "an .anim file or a model clip (<model>#<clip>)" : "an Avatar Mask (.mask)";
    if (!inside || !fs::is_regular_file(absolute, ec)){
        setNotice("\"" + typed + "\": no such file in the project's assets (expected " + what + ")", true);
        return false;
    }
    if (subClip){
        std::vector<anim::ModelClipInfo> infos;
        std::string listError;
        bool found = false;
        if (anim::isModelFilePath(relative) && anim::listModelFileClips(relative, infos, &listError)){
            for (const anim::ModelClipInfo& info : infos) found = found || info.name == reference.clip || std::to_string(info.index) == reference.clip;
        }
        if (!found){
            setNotice("\"" + typed + "\": the model has no animation \"" + reference.clip + "\"", true);
            return false;
        }
        assetRelative = anim::formatSubClipReference(relative, reference.clip);
        return true;
    }
    if (clip ? !Util::isAnimationClipFile(relative) : !Util::isAvatarMaskFile(relative)){
        setNotice("\"" + typed + "\" is not " + what, true);
        return false;
    }
    assetRelative = relative;
    std::string path = relative;
    project->resolveAssetReference(path, guid, true);
    return true;
}

std::string editor::AnimatorWindow::newMergeKey(const char* what){
    return std::string("animator:") + what + ":" + std::to_string(++fieldGesture);
}

const std::vector<ClipWarning>& editor::AnimatorWindow::warnings(){
    if (!doc){
        validation.clear();
        return validation;
    }
    // again at once after an edit of the controller; after a clip changed in the pool (an edit,
    // a reload) within a quarter second; and once a second for files that appeared or went
    // (the Inspector's pace)
    const auto now = std::chrono::steady_clock::now();
    const auto since = now - validationTime;
    const bool poolChanged = AnimationClipPool::revision() != validationPoolRevision;
    if (doc->revision() != validationRevision || (poolChanged && since >= std::chrono::milliseconds(250)) || since >= std::chrono::seconds(1)){
        validationRevision = doc->revision();
        validationTime = now;
        validation = doc->loadWarnings();
        std::vector<ClipWarning> found = validateControllerClips(doc->data());
        validation.insert(validation.end(), found.begin(), found.end());
        // after the loads the validation made itself
        validationPoolRevision = AnimationClipPool::revision();
    }
    return validation;
}

bool editor::AnimatorWindow::commit(anim::ControllerData data, const std::string& undoName, const std::string& mergeKey){
    if (!doc){
        setNotice("No Animator Controller is open", true);
        return false;
    }
    std::string commitError;
    if (!doc->commit(data, undoName, mergeKey, &commitError)){
        setNotice(commitError, true);
        return false;
    }
    noticeText.clear();
    syncLayers();
    return true;
}

// ---- selection ----

void editor::AnimatorWindow::select(const animator::Item& item, bool add){
    if (item.kind == animator::ItemKind::State){
        if (!add) selectedStateList.clear();
        if (std::find(selectedStateList.begin(), selectedStateList.end(), item.index) == selectedStateList.end()){
            selectedStateList.push_back(item.index);
        }
    }else{
        selectedStateList.clear();
    }
    selected = item;
    if (item.layer >= 0 && item.layer != layer && doc && (size_t)item.layer < doc->data().layers.size()){
        layer = item.layer;
        framePending = true;
    }
}

void editor::AnimatorWindow::selectStates(const std::vector<int>& states){
    selectedStateList = states;
    selected = states.empty() ? animator::Item() : animator::Item{animator::ItemKind::State, layer, states.back()};
}

void editor::AnimatorWindow::clearSelection(){
    selected = animator::Item();
    selectedStateList.clear();
}

void editor::AnimatorWindow::selectPath(const std::string& jsonPath){
    const animator::Item item = animator::itemOfPath(jsonPath);
    if (item.kind == animator::ItemKind::None) return;
    if (item.layer >= 0) setCurrentLayer(item.layer);
    select(item);
    if (item.kind == animator::ItemKind::State) frameSelection();
}

// ---- edits ----

int editor::AnimatorWindow::createState(const std::string& clip, bool blendTree, const std::array<float, 2>* at){
    if (!doc) return -1;
    anim::ControllerData data = doc->data();
    anim::ControllerState state;
    if (!clip.empty()){
        state.clip = clip;
        state.name = clipDisplayName(clip);
        state.clipGuid = clipGuidOf(clip);
    }else if (blendTree){
        state.name = "Blend Tree";
        state.hasBlendTree = true;
        // Unity makes a "Blend" float parameter for a new tree when there is no number one
        for (const anim::ControllerParameter& parameter : data.parameters){
            if (parameter.type == anim::ControllerParameterType::Float || parameter.type == anim::ControllerParameterType::Int){
                state.blendTree.parameter = parameter.name;
                break;
            }
        }
        if (state.blendTree.parameter.empty()){
            const int index = animator::addParameter(data, anim::ControllerParameterType::Float, "Blend");
            state.blendTree.parameter = data.parameters[(size_t)index].name;
        }
    }
    if (at){
        // centred on the clicked point
        state.position[0] = std::round((*at)[0] - animator::STATE_WIDTH * 0.5f);
        state.position[1] = std::round((*at)[1] - animator::STATE_HEIGHT * 0.5f);
        state.hasPosition = true;
    }
    const int index = animator::addState(data, layer, state);
    if (index < 0) return -1;
    if (!commit(data, "Create State")) return -1;
    select({animator::ItemKind::State, layer, index});
    return index;
}

int editor::AnimatorWindow::dropClips(const std::vector<std::string>& paths, const std::array<float, 2>& at){
    if (!doc || !project) return 0;
    anim::ControllerData data = doc->data();
    int created = 0;
    std::vector<int> indices;
    for (const std::string& given : paths){
        const fs::path absolute = fs::u8path(given).is_absolute() ? fs::u8path(given) : project->getProjectPath() / fs::u8path(given);
        const std::string relative = project->normalizeToAssetsRelative(absolute).generic_string();
        std::vector<std::string> clips;
        if (Util::isAnimationClipFile(relative)){
            clips.push_back(relative);
        }else if (anim::isModelFilePath(relative)){
            std::vector<anim::ModelClipInfo> infos;
            std::string listError;
            if (anim::listModelFileClips(relative, infos, &listError)){
                for (const anim::ModelClipInfo& info : infos) clips.push_back(anim::formatSubClipReference(relative, info.name));
            }
        }
        for (const std::string& clip : clips){
            anim::ControllerState state;
            state.clip = clip;
            state.name = clipDisplayName(clip);
            state.clipGuid = clipGuidOf(clip);
            state.position[0] = std::round(at[0]);
            state.position[1] = std::round(at[1] + animator::GRID_Y * (float)created);
            state.hasPosition = true;
            indices.push_back(animator::addState(data, layer, state));
            created++;
        }
    }
    if (created == 0){
        setNotice("Drop .anim files or models with animations", true);
        return 0;
    }
    if (!commit(data, created == 1 ? "Create State" : "Create States")) return 0;
    selectStates(indices);
    return created;
}

bool editor::AnimatorWindow::makeTransition(int from, int to){
    if (!doc) return false;
    anim::ControllerData data = doc->data();
    const int index = animator::addTransition(data, layer, from, to);
    if (index < 0){
        setNotice("A transition goes to a state", true);
        return false;
    }
    if (!commit(data, "Make Transition")) return false;
    select({animator::ItemKind::Transition, layer, index});
    return true;
}

bool editor::AnimatorWindow::setDefaultState(int state){
    if (!doc) return false;
    anim::ControllerData data = doc->data();
    std::string editError;
    if (!animator::setDefaultState(data, layer, state, &editError)){
        setNotice(editError, true);
        return false;
    }
    return commit(data, "Set as Layer Default State");
}

bool editor::AnimatorWindow::deleteSelection(){
    if (!doc) return false;
    anim::ControllerData data = doc->data();
    if (!selectedStateList.empty()){
        animator::removeStates(data, layer, selectedStateList);
        clearSelection();
        return commit(data, "Delete States");
    }
    if (selected.kind == animator::ItemKind::Transition){
        animator::removeTransitions(data, layer, {selected.index});
        clearSelection();
        return commit(data, "Delete Transition");
    }
    if (selected.kind == animator::ItemKind::Parameter){
        const int parameter = selected.index;
        clearSelection();
        return removeParameter(parameter);
    }
    return false;
}

bool editor::AnimatorWindow::copySelection(){
    if (!doc || selectedStateList.empty()) return false;
    clipboard = animator::copyStates(doc->data(), layer, selectedStateList);
    return !clipboard.empty();
}

bool editor::AnimatorWindow::paste(const std::array<float, 2>& at){
    if (!doc || clipboard.empty()) return false;
    anim::ControllerData data = doc->data();
    const std::vector<int> pasted = animator::pasteStates(data, layer, clipboard, at);
    if (!commit(data, "Paste States")) return false;
    selectStates(pasted);
    return true;
}

bool editor::AnimatorWindow::renameState(int state, const std::string& name){
    if (!doc) return false;
    anim::ControllerData data = doc->data();
    std::string editError;
    if (!animator::renameState(data, layer, state, name, &editError)){
        setNotice(editError, true);
        return false;
    }
    return commit(data, "Rename State");
}

bool editor::AnimatorWindow::editState(int state, const std::function<void(anim::ControllerState&)>& edit, const std::string& mergeKey){
    const anim::ControllerLayer* data = currentLayerData();
    if (!data || state < 0 || (size_t)state >= data->states.size()) return false;
    anim::ControllerData copy = doc->data();
    edit(copy.layers[(size_t)layer].states[(size_t)state]);
    return commit(copy, "Edit State", mergeKey);
}

bool editor::AnimatorWindow::editTransition(int transition, const std::function<void(anim::ControllerTransition&)>& edit, const std::string& mergeKey){
    const anim::ControllerLayer* data = currentLayerData();
    if (!data || transition < 0 || (size_t)transition >= data->transitions.size()) return false;
    anim::ControllerData copy = doc->data();
    edit(copy.layers[(size_t)layer].transitions[(size_t)transition]);
    return commit(copy, "Edit Transition", mergeKey);
}

bool editor::AnimatorWindow::editLayer(int index, const std::function<void(anim::ControllerLayer&)>& edit, const std::string& mergeKey){
    if (!doc || index < 0 || (size_t)index >= doc->data().layers.size()) return false;
    anim::ControllerData copy = doc->data();
    edit(copy.layers[(size_t)index]);
    return commit(copy, "Edit Layer", mergeKey);
}

void editor::AnimatorWindow::beginDrag(){
    dragging = true;
    dragGesture++;
}

bool editor::AnimatorWindow::dragStates(const std::vector<int>& states, float dx, float dy){
    const anim::ControllerLayer* layerData = currentLayerData();
    if (!layerData || states.empty() || (dx == 0.0f && dy == 0.0f)) return false;
    anim::ControllerData data = doc->data();
    anim::ControllerLayer& target = data.layers[(size_t)layer];
    for (int state : states){
        if (state < 0 || (size_t)state >= target.states.size()) continue;
        const std::array<float, 2> position = animator::displayPosition(target, state);
        target.states[(size_t)state].position[0] = std::round(position[0] + dx);
        target.states[(size_t)state].position[1] = std::round(position[1] + dy);
        target.states[(size_t)state].hasPosition = true;
    }
    // one undo step per drag gesture (merged)
    return commit(data, "Move States", "animator:drag:" + std::to_string(dragGesture));
}

bool editor::AnimatorWindow::dragAnyState(float dx, float dy){
    const anim::ControllerLayer* layerData = currentLayerData();
    if (!layerData || (dx == 0.0f && dy == 0.0f)) return false;
    anim::ControllerData data = doc->data();
    anim::ControllerLayer& target = data.layers[(size_t)layer];
    const animator::Rect rect = animator::anyStateRect(target);
    target.anyStatePosition[0] = std::round(rect.x + dx);
    target.anyStatePosition[1] = std::round(rect.y + dy);
    target.hasAnyStatePosition = true;
    return commit(data, "Move Any State", "animator:drag:" + std::to_string(dragGesture));
}

void editor::AnimatorWindow::endDrag(){
    dragging = false;
    dragGesture++;
}

int editor::AnimatorWindow::addParameter(anim::ControllerParameterType type){
    if (!doc) return -1;
    anim::ControllerData data = doc->data();
    const int index = animator::addParameter(data, type, "");
    if (!commit(data, "Add Parameter")) return -1;
    select({animator::ItemKind::Parameter, -1, index});
    return index;
}

bool editor::AnimatorWindow::renameParameter(int parameter, const std::string& name){
    if (!doc) return false;
    anim::ControllerData data = doc->data();
    std::string editError;
    if (!animator::renameParameter(data, parameter, name, &editError)){
        setNotice(editError, true);
        return false;
    }
    return commit(data, "Rename Parameter");
}

bool editor::AnimatorWindow::setParameterDefault(int parameter, float value, const std::string& mergeKey){
    if (!doc || parameter < 0 || (size_t)parameter >= doc->data().parameters.size()) return false;
    anim::ControllerData data = doc->data();
    data.parameters[(size_t)parameter].defaultValue = value;
    return commit(data, "Edit Parameter", mergeKey);
}

bool editor::AnimatorWindow::removeParameter(int parameter){
    if (!doc || parameter < 0 || (size_t)parameter >= doc->data().parameters.size()) return false;
    anim::ControllerData data = doc->data();
    animator::removeParameter(data, parameter);
    if (selected.kind == animator::ItemKind::Parameter) clearSelection();
    return commit(data, "Remove Parameter");
}

int editor::AnimatorWindow::addLayer(){
    if (!doc) return -1;
    anim::ControllerData data = doc->data();
    const int index = animator::addLayer(data, "");
    if (!commit(data, "Add Layer")) return -1;
    setCurrentLayer(index);
    return index;
}

bool editor::AnimatorWindow::removeLayer(int index){
    if (!doc) return false;
    anim::ControllerData data = doc->data();
    std::string editError;
    if (!animator::removeLayer(data, index, &editError)){
        setNotice(editError, true);
        return false;
    }
    // the current layer stays by name (the removed one: the base layer), syncLayers
    return commit(data, "Remove Layer");
}

bool editor::AnimatorWindow::moveLayer(int from, int to){
    if (!doc) return false;
    anim::ControllerData data = doc->data();
    std::string editError;
    if (!animator::moveLayer(data, from, to, &editError)){
        setNotice(editError, true);
        return false;
    }
    return commit(data, "Move Layer");
}

bool editor::AnimatorWindow::renameLayer(int index, const std::string& name){
    if (!doc) return false;
    anim::ControllerData data = doc->data();
    std::string editError;
    if (!animator::renameLayer(data, index, name, &editError)){
        setNotice(editError, true);
        return false;
    }
    return commit(data, "Rename Layer");
}

bool editor::AnimatorWindow::isLive() const{
    SceneProject* sceneProject = boundSceneProject();
    AnimationPlayerComponent* player = boundPlayer();
    return sceneProject && player && sceneProject->playState != ScenePlayState::STOPPED && player->runtime &&
           player->runtime->controller && player->runtime->controller->controller;
}

bool editor::AnimatorWindow::setLiveParameter(const std::string& name, float value){
    AnimationPlayerComponent* player = boundPlayer();
    if (!player || !player->runtime || !player->runtime->controller || !player->runtime->controller->controller){
        setNotice("No running Animation Player with this controller", true);
        return false;
    }
    const anim::ControllerData& data = player->runtime->controller->controller->getData();
    const int index = data.findParameter(name);
    if (index < 0) return false;
    SceneProject* sceneProject = boundSceneProject();
    const std::string who = sceneProject ? sceneProject->scene->getEntityName(entity) : std::string();
    return anim::setControllerParameter(*player, name, data.parameters[(size_t)index].type, value, 0.0f, who);
}

// ---- framing ----

void editor::AnimatorWindow::frameSelection(float width, float height){
    const anim::ControllerLayer* data = currentLayerData();
    if (!data) return;
    std::vector<animator::Rect> rects;
    for (int state : selectedStateList) rects.push_back(animator::stateRect(*data, state));
    if (selected.kind == animator::ItemKind::AnyState) rects.push_back(animator::anyStateRect(*data));
    if (selected.kind == animator::ItemKind::Transition && (size_t)selected.index < data->transitions.size()){
        const anim::ControllerTransition& transition = data->transitions[(size_t)selected.index];
        const int from = data->findState(transition.from);
        rects.push_back(from >= 0 ? animator::stateRect(*data, from) : animator::anyStateRect(*data));
        rects.push_back(animator::stateRect(*data, data->findState(transition.to)));
    }
    if (rects.empty()){
        frameAll(width, height);
        return;
    }
    animator::Rect content;
    animator::bounds(rects, content);
    animator::frame(content, width > 0.0f ? width : canvasWidth, height > 0.0f ? height : canvasHeight, 40.0f, MIN_ZOOM, 1.0f, panX, panY, viewZoom);
}

void editor::AnimatorWindow::frameAll(float width, float height){
    const anim::ControllerLayer* data = currentLayerData();
    if (!data) return;
    std::vector<animator::Rect> rects = {animator::anyStateRect(*data)};
    for (size_t i = 0; i < data->states.size(); i++) rects.push_back(animator::stateRect(*data, (int)i));
    animator::Rect content;
    animator::bounds(rects, content);
    animator::frame(content, width > 0.0f ? width : canvasWidth, height > 0.0f ? height : canvasHeight, 40.0f, MIN_ZOOM, 1.0f, panX, panY, viewZoom);
}

// ---- preview ----

bool editor::AnimatorWindow::startPreview(){
    SceneProject* sceneProject = boundSceneProject();
    const anim::ControllerLayer* data = currentLayerData();
    if (!sceneProject || !boundPlayer() || sceneProject->playState != ScenePlayState::STOPPED){
        setNotice("Select a GameObject whose Animation Player uses this controller (edit mode) to preview on it", true);
        return false;
    }
    if (!data || selectedStateList.size() != 1 || data->states[(size_t)selectedStateList[0]].isEmpty()){
        setNotice("Select a state with a clip or a blend tree to preview", true);
        return false;
    }
    if (!previewing){
        previewToken = PreviewOwner::acquire(sceneId, entity, sceneProject->scene, "Animator window", [this](){
            // another owner takes the root: our pose goes back first
            if (SceneProject* owner = boundSceneProject()){
                owner->scene->getSystem<ActionSystem>()->restorePlayerPreview(previewSession);
            }
            previewing = false;
            previewToken = 0;
        });
        previewSession.reset();
        previewTime = 0.0f;
        previewing = true;
    }
    updatePreview(0.0f);
    return true;
}

void editor::AnimatorWindow::stopPreview(){
    if (!previewing) return;
    previewing = false;
    if (SceneProject* sceneProject = boundSceneProject()){
        sceneProject->scene->getSystem<ActionSystem>()->restorePlayerPreview(previewSession);
    }
    previewSession.reset();
    PreviewOwner::Token token = previewToken;
    previewToken = 0;
    PreviewOwner::release(token);
}

void editor::AnimatorWindow::updatePreview(float dt){
    if (!previewing) return;
    SceneProject* sceneProject = boundSceneProject();
    const anim::ControllerLayer* data = currentLayerData();
    if (!sceneProject || sceneProject->playState != ScenePlayState::STOPPED || !PreviewOwner::owns(previewToken) || !data ||
        selectedStateList.size() != 1 || (size_t)selectedStateList[0] >= data->states.size()){
        stopPreview();
        return;
    }
    const anim::ControllerState& state = data->states[(size_t)selectedStateList[0]];
    ActionSystem::PlayerPreview preview;
    if (!state.clip.empty()){
        previewTime += dt * state.speed;
        ActionSystem::PlayerPreviewState entry;
        entry.clip = AnimationClipPool::getOrLoad(state.clip);
        entry.time = previewTime;
        entry.wrap = state.wrap;
        entry.layer = layer;
        if (entry.clip) preview.states.push_back(entry);
    }else if (state.hasBlendTree){
        std::vector<float> thresholds;
        for (const anim::BlendTreeChild& child : state.blendTree.children) thresholds.push_back(child.threshold);
        std::vector<float> weights;
        anim::blendTreeWeights(thresholds, blendValue, weights);
        // the children in step: one normalized time at the weighted rate
        double rate = 0.0;
        std::vector<std::shared_ptr<const AnimationClip>> clips;
        for (size_t c = 0; c < state.blendTree.children.size(); c++){
            clips.push_back(AnimationClipPool::getOrLoad(state.blendTree.children[c].clip));
            if (clips.back() && clips.back()->getLength() > 0.0f) rate += weights[c] * state.blendTree.children[c].speed / clips.back()->getLength();
        }
        previewTime += (float)(dt * state.speed * rate);
        for (size_t c = 0; c < clips.size(); c++){
            if (!clips[c] || !(weights[c] > 0.0f)) continue;
            ActionSystem::PlayerPreviewState entry;
            entry.clip = clips[c];
            entry.time = previewTime * clips[c]->getLength();
            entry.wrap = state.wrap;
            entry.weight = weights[c];
            entry.layer = layer;
            preview.states.push_back(entry);
        }
    }
    // a state on an upper layer previews through that layer's mask
    if (layer > 0){
        preview.layered = true;
        preview.layers.resize((size_t)layer + 1);
        preview.layers[(size_t)layer].mask = data->mask;
    }
    sceneProject->scene->getSystem<ActionSystem>()->previewPlayer(entity, preview, previewSession);
}

// ---- drawing ----

void editor::AnimatorWindow::show(){
    if (!windowOpen){
        windowFocused = false;
        return;
    }
    followSelection();
    followBoundController();
    if (focusRequested){
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(900.0f), Theme::dpi(520.0f)), ImGuiCond_FirstUseEver);
    bool open = windowOpen;
    const bool visible = ImGui::Begin(WINDOW_NAME, &open);
    windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (visible){
        drawHeader();
        if (doc){
            const float available = ImGui::GetContentRegionAvail().y;
            const float strip = warnings().empty() ? 0.0f : std::min(Theme::dpi(110.0f), available * 0.3f);
            const float body = std::max(Theme::dpi(80.0f), available - strip - (strip > 0.0f ? ImGui::GetStyle().ItemSpacing.y : 0.0f));
            const float leftWidth = Theme::dpi(220.0f);
            const float rightWidth = Theme::dpi(300.0f);
            const float canvas = std::max(Theme::dpi(120.0f), ImGui::GetContentRegionAvail().x - leftWidth - rightWidth - 2.0f * ImGui::GetStyle().ItemSpacing.x);
            drawLeftPanel(leftWidth, body);
            ImGui::SameLine();
            drawCanvas(canvas, body);
            ImGui::SameLine();
            drawInspector(rightWidth, body);
            if (strip > 0.0f) drawValidationStrip();
        }
    }
    ImGui::End();
    if (!open) setOpen(false);
    updatePreview(ImGui::GetIO().DeltaTime);
}

void editor::AnimatorWindow::drawHeader(){
    if (!doc){
        if (!error.empty()){
            ImGui::TextColored(Theme::Colors::ErrorText, ICON_FA_CIRCLE_XMARK " %s", error.c_str());
        }else{
            ImGui::TextDisabled("Select a GameObject whose Animation Player has an Animator Controller, or double-click a .controller in the Project window.");
            ImGui::TextDisabled("Assets > Create > Animator Controller makes one; the Inspector's Create Controller from Clips makes one from a player's clips.");
        }
        return;
    }
    ImGui::TextUnformatted(ICON_FA_SITEMAP);
    ImGui::SameLine();
    ImGui::TextUnformatted(doc->path().c_str());
    UIProbe::item("animator.path");
    if (!doc->exists()){
        ImGui::SameLine();
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " Missing");
    }
    if (SceneProject* sceneProject = boundSceneProject()){
        if (entity != NULL_ENTITY && sceneProject->scene->isEntityCreated(entity)){
            ImGui::SameLine();
            ImGui::TextDisabled("on %s%s", sceneProject->scene->getEntityName(entity).c_str(), isLive() ? " (live)" : "");
        }
    }
    ImGui::SameLine();
    const bool canPreview = !isLive() && boundPlayer() != nullptr;
    ImGui::BeginDisabled(!canPreview);
    if (previewing){
        if (ImGui::SmallButton(ICON_FA_STOP " Stop Preview")) stopPreview();
    }else if (ImGui::SmallButton(ICON_FA_PLAY " Preview")){
        startPreview();
    }
    UIProbe::item("animator.preview");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Plays the selected state (a blend tree at its scrubbed value) on the bound GameObject in the Scene view (edit mode)");
    if (!noticeText.empty()){
        ImGui::SameLine();
        ImGui::TextColored(noticeError ? Theme::Colors::WarningText : Theme::Colors::SubtleText, "%s", noticeText.c_str());
    }
}

void editor::AnimatorWindow::drawLeftPanel(float width, float height){
    ImGui::BeginChild("##animator_left", ImVec2(width, height), ImGuiChildFlags_Borders);
    if (ImGui::BeginTabBar("##animator_tabs")){
        if (ImGui::BeginTabItem("Layers")){
            drawLayersTab();
            ImGui::EndTabItem();
        }
        const bool parametersTab = ImGui::BeginTabItem("Parameters");
        UIProbe::item("animator.tab.parameters");
        if (parametersTab){
            drawParametersTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

void editor::AnimatorWindow::drawLayersTab(){
    // a copy: an edit below replaces the document's data
    const anim::ControllerData data = doc->data();
    if (ImGui::SmallButton(ICON_FA_PLUS " Layer")) addLayer();
    UIProbe::item("animator.add_layer");
    for (size_t l = 0; l < data.layers.size(); l++){
        const anim::ControllerLayer& layerData = data.layers[l];
        ImGui::PushID((int)l);
        if (ImGui::Selectable(layerData.name.c_str(), (int)l == layer, 0, ImVec2(ImGui::GetContentRegionAvail().x - Theme::dpi(60.0f), 0))){
            setCurrentLayer((int)l);
        }
        UIProbe::item("animator.layer." + std::to_string(l));
        if (l > 0 && ImGui::BeginDragDropSource()){
            const int index = (int)l;
            ImGui::SetDragDropPayload("animator_layer", &index, sizeof(index));
            ImGui::TextUnformatted(layerData.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (l > 0 && ImGui::BeginDragDropTarget()){
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("animator_layer")){
                moveLayer(*static_cast<const int*>(payload->Data), (int)l);
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_GEAR)) ImGui::OpenPopup("layer_settings");
        ImGui::SetItemTooltip("Weight, blending and mask");
        if (ImGui::BeginPopup("layer_settings")){
            char name[128];
            std::snprintf(name, sizeof(name), "%s", layerData.name.c_str());
            ImGui::SetNextItemWidth(Theme::dpi(180.0f));
            ImGui::InputText("Name", name, sizeof(name));
            if (ImGui::IsItemDeactivatedAfterEdit()) renameLayer((int)l, name);
            if (l == 0){
                ImGui::BeginDisabled();
                float one = 1.0f;
                ImGui::SliderFloat("Weight", &one, 0.0f, 1.0f, "1 (Base)");
                ImGui::TextDisabled("Blending: Override (the base layer)");
                ImGui::EndDisabled();
            }else{
                static std::string weightKey;
                float weight = layerData.weight;
                ImGui::SetNextItemWidth(Theme::dpi(180.0f));
                if (ImGui::SliderFloat("Weight", &weight, 0.0f, 1.0f)){
                    if (ImGui::IsItemActivated() || weightKey.empty()) weightKey = newMergeKey("layer_weight");
                    editLayer((int)l, [weight](anim::ControllerLayer& target){ target.weight = weight; }, weightKey);
                }
                if (ImGui::IsItemDeactivated()) weightKey.clear();
                int blend = layerData.blend == AnimationLayerBlend::Additive ? 1 : 0;
                const char* blends[] = {"Override", "Additive"};
                ImGui::SetNextItemWidth(Theme::dpi(180.0f));
                if (ImGui::Combo("Blending", &blend, blends, 2)){
                    editLayer((int)l, [blend](anim::ControllerLayer& target){
                        target.blend = blend == 1 ? AnimationLayerBlend::Additive : AnimationLayerBlend::Override;
                    });
                }
            }
            char mask[512];
            std::snprintf(mask, sizeof(mask), "%s", layerData.mask.c_str());
            ImGui::SetNextItemWidth(Theme::dpi(180.0f));
            ImGui::InputTextWithHint("Mask", "none (drop a .mask)", mask, sizeof(mask));
            if (ImGui::IsItemDeactivatedAfterEdit()){
                std::string value, guid;
                if (resolveTypedAsset(mask, false, value, guid)){
                    editLayer((int)l, [value, guid](anim::ControllerLayer& target){
                        target.mask = value;
                        target.maskGuid = guid;
                    });
                }
            }
            if (ImGui::BeginDragDropTarget()){
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
                    const std::vector<std::string> paths = Util::getStringsFromPayload(payload);
                    if (!paths.empty() && Util::isAvatarMaskFile(paths.front())){
                        const fs::path absolute = fs::u8path(paths.front());
                        const std::string value = project->normalizeToAssetsRelative(absolute).generic_string();
                        const std::string guid = project->getAssetDatabase().getGuid(absolute);
                        editLayer((int)l, [value, guid](anim::ControllerLayer& target){
                            target.mask = value;
                            target.maskGuid = guid;
                        });
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (!layerData.mask.empty()){
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_XMARK)){
                    editLayer((int)l, [](anim::ControllerLayer& target){
                        target.mask.clear();
                        target.maskGuid.clear();
                    });
                }
            }
            if (l > 0){
                ImGui::Separator();
                if (l > 1 && ImGui::MenuItem("Move Up")) moveLayer((int)l, (int)l - 1);
                if (l + 1 < data.layers.size() && ImGui::MenuItem("Move Down")) moveLayer((int)l, (int)l + 1);
                if (ImGui::MenuItem("Remove Layer")){
                    removeLayer((int)l);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        // in play mode: the layer's live weight
        if (isLive()){
            AnimationPlayerComponent* player = boundPlayer();
            const float weight = player->runtime->layerWeight((int)l);
            ImGui::ProgressBar(weight, ImVec2(-1.0f, Theme::dpi(4.0f)), "");
        }
        ImGui::PopID();
    }
}

void editor::AnimatorWindow::drawParametersTab(){
    const anim::ControllerData data = doc->data();
    if (ImGui::SmallButton(ICON_FA_PLUS)) ImGui::OpenPopup("add_parameter");
    UIProbe::item("animator.add_parameter");
    ImGui::SetItemTooltip("Add a parameter");
    if (ImGui::BeginPopup("add_parameter")){
        if (ImGui::MenuItem("Float")) addParameter(anim::ControllerParameterType::Float);
        UIProbe::item("animator.add_parameter.float");
        if (ImGui::MenuItem("Int")) addParameter(anim::ControllerParameterType::Int);
        UIProbe::item("animator.add_parameter.int");
        if (ImGui::MenuItem("Bool")) addParameter(anim::ControllerParameterType::Bool);
        UIProbe::item("animator.add_parameter.bool");
        if (ImGui::MenuItem("Trigger")) addParameter(anim::ControllerParameterType::Trigger);
        UIProbe::item("animator.add_parameter.trigger");
        ImGui::EndPopup();
    }
    const bool live = isLive();
    AnimationPlayerComponent* player = live ? boundPlayer() : nullptr;
    const anim::ControllerRuntime* runtime = player ? player->runtime->controller.get() : nullptr;
    if (live) ImGui::TextDisabled("Live values (not saved)");
    static std::string defaultKey;
    for (size_t p = 0; p < data.parameters.size(); p++){
        const anim::ControllerParameter& parameter = data.parameters[p];
        ImGui::PushID((int)p);
        const float valueWidth = Theme::dpi(70.0f);
        if (parameterRename == (int)p){
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - valueWidth);
            if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
            const bool done = ImGui::InputText("##rename", renameBuffer, sizeof(renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
            if (done || ImGui::IsItemDeactivated()){
                if (!ImGui::IsKeyPressed(ImGuiKey_Escape)) renameParameter((int)p, renameBuffer);
                parameterRename = -1;
            }
        }else{
            const bool isSelected = selected.kind == animator::ItemKind::Parameter && selected.index == (int)p;
            if (ImGui::Selectable(parameter.name.c_str(), isSelected, ImGuiSelectableFlags_AllowDoubleClick,
                                  ImVec2(ImGui::GetContentRegionAvail().x - valueWidth, 0))){
                select({animator::ItemKind::Parameter, -1, (int)p});
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)){
                    parameterRename = (int)p;
                    std::snprintf(renameBuffer, sizeof(renameBuffer), "%s", parameter.name.c_str());
                }
            }
            if (ImGui::BeginPopupContextItem()){
                if (ImGui::MenuItem("Rename")){
                    parameterRename = (int)p;
                    std::snprintf(renameBuffer, sizeof(renameBuffer), "%s", parameter.name.c_str());
                }
                if (ImGui::MenuItem("Delete")) removeParameter((int)p);
                ImGui::EndPopup();
            }
            ImGui::SetItemTooltip("%s (%s): double-click to rename", parameter.name.c_str(), anim::parameterTypeName(parameter.type));
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(valueWidth - ImGui::GetStyle().ItemSpacing.x);
        // the live value in play mode (the runtime's), else the default (the file's)
        const int runtimeIndex = runtime ? runtime->controller->getData().findParameter(parameter.name) : -1;
        float value = runtimeIndex >= 0 ? runtime->params[(size_t)runtimeIndex].value : parameter.defaultValue;
        bool changed = false;
        switch (parameter.type){
            case anim::ControllerParameterType::Float:
                changed = ImGui::DragFloat("##value", &value, 0.01f);
                break;
            case anim::ControllerParameterType::Int: {
                int number = (int)std::round(value);
                changed = ImGui::DragInt("##value", &number);
                value = (float)number;
                break;
            }
            case anim::ControllerParameterType::Bool: {
                bool flag = value != 0.0f;
                changed = ImGui::Checkbox("##value", &flag);
                value = flag ? 1.0f : 0.0f;
                break;
            }
            case anim::ControllerParameterType::Trigger:
                if (live){
                    if (value != 0.0f) ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), ICON_FA_BOLT " set");
                    else if (ImGui::SmallButton(ICON_FA_BOLT)){
                        value = 1.0f;
                        changed = true;
                    }
                }else{
                    ImGui::TextDisabled("trigger");
                }
                break;
        }
        UIProbe::item("animator.parameter." + std::to_string(p) + ".value");
        if (changed){
            if (live){
                setLiveParameter(parameter.name, value);
            }else{
                if (ImGui::IsItemActivated() || defaultKey.empty()) defaultKey = newMergeKey("parameter_default");
                setParameterDefault((int)p, value, defaultKey);
            }
        }
        if (ImGui::IsItemDeactivated()) defaultKey.clear();
        ImGui::PopID();
    }
}

void editor::AnimatorWindow::drawCanvas(float width, float height){
    ImGui::BeginChild("##animator_canvas", ImVec2(width, height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    // a copy: an edit below (a drop, a transition, a drag's end) replaces the document's data
    anim::ControllerLayer layerCopy;
    if (const anim::ControllerLayer* current = currentLayerData()) layerCopy = *current;
    const anim::ControllerLayer* data = currentLayerData() ? &layerCopy : nullptr;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    canvasWidth = std::max(1.0f, size.x);
    canvasHeight = std::max(1.0f, size.y);
    if (framePending && data){
        framePending = false;
        frameAll(canvasWidth, canvasHeight);
    }
    ImGui::InvisibleButton("##canvas", ImVec2(canvasWidth, canvasHeight),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    UIProbe::item("animator.canvas");
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    auto toScreen = [&](float x, float y){ return ImVec2(origin.x + (x - panX) * viewZoom, origin.y + (y - panY) * viewZoom); };
    auto toCanvasX = [&](float x){ return panX + (x - origin.x) / viewZoom; };
    auto toCanvasY = [&](float y){ return panY + (y - origin.y) / viewZoom; };
    const float mouseX = toCanvasX(io.MousePos.x), mouseY = toCanvasY(io.MousePos.y);

    // background grid
    drawList->AddRectFilled(origin, ImVec2(origin.x + canvasWidth, origin.y + canvasHeight), IM_COL32(40, 40, 42, 255));
    const float step = 40.0f * viewZoom;
    if (step > 6.0f){
        for (float x = std::fmod(-panX * viewZoom, step); x < canvasWidth; x += step){
            drawList->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + canvasHeight), IM_COL32(52, 52, 55, 255));
        }
        for (float y = std::fmod(-panY * viewZoom, step); y < canvasHeight; y += step){
            drawList->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + canvasWidth, origin.y + y), IM_COL32(52, 52, 55, 255));
        }
    }
    if (!data){
        ImGui::EndChild();
        return;
    }

    // live state of the bound player (play mode)
    const bool live = isLive();
    int liveCurrent = -1, liveNext = -1;
    float liveTime = 0.0f, liveProgress = 0.0f;
    if (live){
        const anim::ControllerRuntime& runtime = *boundPlayer()->runtime->controller;
        const anim::ControllerData& runtimeData = runtime.controller->getData();
        const int runtimeLayer = runtimeData.findLayer(data->name);
        if (runtimeLayer >= 0 && (size_t)runtimeLayer < runtime.layers.size()){
            const anim::ControllerLayerRuntime& layerRuntime = runtime.layers[(size_t)runtimeLayer];
            const anim::ControllerLayer& runtimeLayerData = runtimeData.layers[(size_t)runtimeLayer];
            if (layerRuntime.started && layerRuntime.current >= 0){
                liveCurrent = data->findState(runtimeLayerData.states[(size_t)layerRuntime.current].name);
                liveTime = (float)layerRuntime.groups[(size_t)layerRuntime.current].time;
            }
            if (layerRuntime.transition.active && layerRuntime.transition.to >= 0){
                liveNext = data->findState(runtimeLayerData.states[(size_t)layerRuntime.transition.to].name);
                liveProgress = layerRuntime.transition.duration > 0.0f ? layerRuntime.transition.elapsed / layerRuntime.transition.duration : 1.0f;
            }
        }
    }

    // the drag in progress moves the selected boxes on screen (committed on release)
    static float dragDX = 0.0f, dragDY = 0.0f;
    static bool dragAny = false;
    auto boxOf = [&](int state){
        animator::Rect rect = state == -2 ? animator::anyStateRect(*data) : animator::stateRect(*data, state);
        const bool moving = dragging && (state == -2 ? dragAny : std::find(selectedStateList.begin(), selectedStateList.end(), state) != selectedStateList.end());
        if (moving){
            rect.x += dragDX;
            rect.y += dragDY;
        }
        return rect;
    };

    // arrows
    const std::vector<animator::Arrow> arrows = animator::arrows(*data);
    for (const animator::Arrow& arrow : arrows){
        const animator::Rect a = boxOf(arrow.from < 0 ? -2 : arrow.from);
        const animator::Rect b = boxOf(arrow.to);
        const animator::Rect a0 = arrow.from < 0 ? animator::anyStateRect(*data) : animator::stateRect(*data, arrow.from);
        const animator::Rect b0 = animator::stateRect(*data, arrow.to);
        const ImVec2 p0 = toScreen(arrow.ax + (a.x - a0.x), arrow.ay + (a.y - a0.y));
        const ImVec2 p1 = toScreen(arrow.bx + (b.x - b0.x), arrow.by + (b.y - b0.y));
        bool isSelected = false;
        for (int t : arrow.transitions) isSelected = isSelected || (selected.kind == animator::ItemKind::Transition && selected.index == t);
        const bool active = live && liveNext == arrow.to && (arrow.from < 0 || arrow.from == liveCurrent);
        const ImU32 color = active ? LIVE_COLOR : isSelected ? ARROW_SELECTED : ARROW_COLOR;
        const float thickness = (isSelected || active ? 3.0f : 2.0f) * std::max(0.6f, viewZoom);
        if (arrow.self){
            drawList->AddBezierCubic(p0, ImVec2(p0.x + 30.0f * viewZoom, p0.y), ImVec2(p1.x + 30.0f * viewZoom, p1.y), p1, color, thickness);
            drawArrowHead(drawList, p1, ImVec2(-1.0f, 0.0f), 5.0f * std::max(0.6f, viewZoom), color);
        }else{
            drawList->AddLine(p0, p1, color, thickness);
            const ImVec2 middle((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
            drawArrowHead(drawList, middle, ImVec2(p1.x - p0.x, p1.y - p0.y), 6.0f * std::max(0.6f, viewZoom), color);
            if (arrow.transitions.size() > 1){
                char badge[16];
                std::snprintf(badge, sizeof(badge), "x%zu", arrow.transitions.size());
                drawList->AddText(ImVec2(middle.x + 8.0f, middle.y - 16.0f), color, badge);
            }
            if (active){
                // the transition's progress runs along the arrow
                const float t = std::min(1.0f, std::max(0.0f, liveProgress));
                drawList->AddCircleFilled(ImVec2(p0.x + (p1.x - p0.x) * t, p0.y + (p1.y - p0.y) * t), 5.0f, LIVE_COLOR);
            }
        }
    }

    // boxes: the Any State, then the states
    auto drawBox = [&](int state){
        const animator::Rect rect = boxOf(state);
        const ImVec2 r0 = toScreen(rect.x, rect.y), r1 = toScreen(rect.x + rect.w, rect.y + rect.h);
        const float rounding = 6.0f * viewZoom;
        std::string label;
        ImU32 fill = STATE_COLOR;
        bool isSelected = false;
        if (state == -2){
            fill = ANY_STATE_COLOR;
            label = "Any State";
            isSelected = selected.kind == animator::ItemKind::AnyState;
        }else{
            const anim::ControllerState& stateData = data->states[(size_t)state];
            label = stateData.name;
            if (data->defaultStateIndex() == state) fill = DEFAULT_STATE_COLOR;
            if (stateData.hasBlendTree) label = std::string(ICON_FA_DIAGRAM_PROJECT " ") + label;
            isSelected = std::find(selectedStateList.begin(), selectedStateList.end(), state) != selectedStateList.end();
        }
        drawList->AddRectFilled(r0, r1, fill, rounding);
        UIProbe::rect(state == -2 ? std::string("animator.any_state") : "animator.state." + std::to_string(state), r0.x, r0.y, r1.x, r1.y);
        if (state >= 0 && (state == liveCurrent || state == liveNext)){
            // the live state: a blue bar of its normalized time
            const float fraction = state == liveCurrent ? liveTime - std::floor(liveTime) : 0.0f;
            const float barTop = r1.y - 5.0f * viewZoom;
            drawList->AddRectFilled(ImVec2(r0.x, barTop), ImVec2(r0.x + (r1.x - r0.x) * fraction, r1.y), LIVE_COLOR, rounding, ImDrawFlags_RoundCornersBottom);
            drawList->AddRect(r0, r1, LIVE_COLOR, rounding, 0, 2.0f);
        }
        if (isSelected) drawList->AddRect(r0, r1, SELECTED_OUTLINE, rounding, 0, 2.5f);
        const float fontSize = ImGui::GetFontSize() * std::min(1.4f, std::max(0.6f, viewZoom));
        const ImVec2 textSize = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label.c_str());
        drawList->PushClipRect(r0, r1, true);
        drawList->AddText(ImGui::GetFont(), fontSize, ImVec2((r0.x + r1.x - textSize.x) * 0.5f, (r0.y + r1.y - textSize.y) * 0.5f),
                          IM_COL32(235, 235, 235, 255), label.c_str());
        drawList->PopClipRect();
    };
    drawBox(-2);
    for (size_t i = 0; i < data->states.size(); i++) drawBox((int)i);

    // Make Transition: a line to the mouse until a target is clicked
    if (linkFrom != -3){
        const animator::Rect source = boxOf(linkFrom < 0 ? -2 : linkFrom);
        drawList->AddLine(toScreen(source.centerX(), source.centerY()), io.MousePos, ARROW_SELECTED, 2.0f);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) linkFrom = -3;
    }

    // input
    if (hovered && io.MouseWheel != 0.0f){
        const float before = viewZoom;
        viewZoom = std::min(MAX_ZOOM, std::max(MIN_ZOOM, viewZoom * (io.MouseWheel > 0.0f ? 1.1f : 1.0f / 1.1f)));
        // zoom at the mouse: the canvas point under it stays
        panX = mouseX - (io.MousePos.x - origin.x) / viewZoom;
        panY = mouseY - (io.MousePos.y - origin.y) / viewZoom;
        (void)before;
    }
    const bool panDrag = ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || (io.KeyAlt && ImGui::IsMouseDragging(ImGuiMouseButton_Left));
    if (ImGui::IsItemActive() && panDrag){
        panX -= io.MouseDelta.x / viewZoom;
        panY -= io.MouseDelta.y / viewZoom;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt){
        const int hit = animator::hitState(*data, mouseX, mouseY);
        if (linkFrom != -3){
            if (hit >= 0) makeTransition(linkFrom, hit);
            linkFrom = -3;
        }else if (hit != -1){
            pressedState = hit;
            pressMoved = false;
            if (hit == -2){
                select({animator::ItemKind::AnyState, layer, -1});
            }else if (io.KeyCtrl || io.KeyShift){
                auto it = std::find(selectedStateList.begin(), selectedStateList.end(), hit);
                if (it != selectedStateList.end()){
                    selectedStateList.erase(it);
                    selected = selectedStateList.empty() ? animator::Item() : animator::Item{animator::ItemKind::State, layer, selectedStateList.back()};
                }else{
                    select({animator::ItemKind::State, layer, hit}, true);
                }
            }else if (std::find(selectedStateList.begin(), selectedStateList.end(), hit) == selectedStateList.end()){
                select({animator::ItemKind::State, layer, hit});
            }
        }else{
            const int arrow = animator::hitArrow(arrows, mouseX, mouseY, 6.0f / viewZoom);
            if (arrow >= 0){
                // several transitions on one arrow: each click selects the next
                const std::vector<int>& list = arrows[(size_t)arrow].transitions;
                auto current = std::find(list.begin(), list.end(), selected.kind == animator::ItemKind::Transition ? selected.index : -1);
                const int next = current == list.end() || current + 1 == list.end() ? list.front() : *(current + 1);
                select({animator::ItemKind::Transition, layer, next});
            }else{
                if (!io.KeyCtrl && !io.KeyShift) clearSelection();
                boxSelecting = true;
                boxStartX = mouseX;
                boxStartY = mouseY;
            }
        }
    }
    if (pressedState != -3 && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !io.KeyAlt){
        if (!pressMoved){
            pressMoved = true;
            beginDrag();
            dragDX = dragDY = 0.0f;
            dragAny = pressedState == -2;
        }
        dragDX += io.MouseDelta.x / viewZoom;
        dragDY += io.MouseDelta.y / viewZoom;
    }
    if (boxSelecting){
        const ImVec2 a = toScreen(boxStartX, boxStartY);
        drawList->AddRectFilled(a, io.MousePos, IM_COL32(90, 170, 255, 40));
        drawList->AddRect(a, io.MousePos, IM_COL32(90, 170, 255, 160));
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)){
        if (pressedState != -3 && pressMoved){
            if (dragAny) dragAnyState(dragDX, dragDY);
            else dragStates(selectedStateList, dragDX, dragDY);
            endDrag();
            dragDX = dragDY = 0.0f;
        }
        pressedState = -3;
        pressMoved = false;
        if (boxSelecting){
            boxSelecting = false;
            const animator::Rect box{std::min(boxStartX, mouseX), std::min(boxStartY, mouseY), std::fabs(mouseX - boxStartX), std::fabs(mouseY - boxStartY)};
            if (box.w > 2.0f || box.h > 2.0f){
                std::vector<int> states = animator::statesIn(*data, box);
                if (io.KeyCtrl || io.KeyShift){
                    for (int s : selectedStateList){
                        if (std::find(states.begin(), states.end(), s) == states.end()) states.push_back(s);
                    }
                }
                selectStates(states);
            }
        }
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)){
        contextAt = {mouseX, mouseY};
        const int hit = animator::hitState(*data, mouseX, mouseY);
        if (hit >= 0){
            if (std::find(selectedStateList.begin(), selectedStateList.end(), hit) == selectedStateList.end()) select({animator::ItemKind::State, layer, hit});
            ImGui::OpenPopup("state_menu");
        }else if (hit == -2){
            select({animator::ItemKind::AnyState, layer, -1});
            ImGui::OpenPopup("any_menu");
        }else{
            const int arrow = animator::hitArrow(arrows, mouseX, mouseY, 6.0f / viewZoom);
            if (arrow >= 0){
                select({animator::ItemKind::Transition, layer, arrows[(size_t)arrow].transitions.front()});
                ImGui::OpenPopup("transition_menu");
            }else{
                ImGui::OpenPopup("canvas_menu");
            }
        }
    }
    // keys: the window focused (not just hovered: Delete in the Hierarchy deletes GameObjects),
    // no text field active
    if (windowFocused && !ImGui::GetIO().WantTextInput){
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) deleteSelection();
        if (ImGui::IsKeyPressed(ImGuiKey_F) && !io.KeyCtrl) frameSelection();
        if (ImGui::IsKeyPressed(ImGuiKey_A) && !io.KeyCtrl) frameAll();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) copySelection();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V)) paste({mouseX, mouseY});
    }
    // drops: clips become states
    if (ImGui::BeginDragDropTarget()){
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
            dropClips(Util::getStringsFromPayload(payload), {mouseX, mouseY});
        }
        ImGui::EndDragDropTarget();
    }
    drawCanvasMenus();
    ImGui::EndChild();
}

void editor::AnimatorWindow::drawCanvasMenus(){
    if (ImGui::BeginPopup("canvas_menu")){
        const bool createMenu = ImGui::BeginMenu("Create State");
        UIProbe::item("animator.menu.create_state");
        if (createMenu){
            if (ImGui::MenuItem("Empty")) createState("", false, &contextAt);
            UIProbe::item("animator.menu.create_empty");
            if (ImGui::MenuItem("From Clip...")){
                pickerAt = contextAt;
                pickerStale = true;
                pickerFilter.clear();
                pickerRequested = true;
            }
            if (ImGui::MenuItem("From New Blend Tree")) createState("", true, &contextAt);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, !clipboard.empty())) paste(contextAt);
        ImGui::Separator();
        if (ImGui::MenuItem("Frame All", "A")) frameAll();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("state_menu")){
        if (ImGui::MenuItem("Make Transition", nullptr, false, selectedStateList.size() == 1)) linkFrom = selectedStateList.front();
        UIProbe::item("animator.menu.make_transition");
        if (ImGui::MenuItem("Set as Layer Default State", nullptr, false, selectedStateList.size() == 1)) setDefaultState(selectedStateList.front());
        if (ImGui::MenuItem("Copy", "Ctrl+C")) copySelection();
        if (ImGui::MenuItem("Delete", "Delete")) deleteSelection();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("any_menu")){
        if (ImGui::MenuItem("Make Transition")) linkFrom = -1;
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("transition_menu")){
        if (ImGui::MenuItem("Delete", "Delete")) deleteSelection();
        ImGui::EndPopup();
    }
    if (pickerRequested){
        pickerRequested = false;
        ImGui::OpenPopup("clip_picker");
    }
    drawClipPicker();
}

void editor::AnimatorWindow::refreshPickerClips(){
    pickerClips.clear();
    pickerStale = false;
    if (!project) return;
    for (const fs::path& file : ProjectChecks::findProjectFiles(project->getAssetsPath(), {".anim", ".gltf", ".glb", ".fbx"})){
        const std::string relative = project->normalizeToAssetsRelative(file).generic_string();
        if (Util::isAnimationClipFile(relative)){
            pickerClips.push_back(relative);
            continue;
        }
        std::vector<anim::ModelClipInfo> infos;
        std::string listError;
        if (anim::listModelFileClips(relative, infos, &listError)){
            for (const anim::ModelClipInfo& info : infos) pickerClips.push_back(anim::formatSubClipReference(relative, info.name));
        }
    }
    std::sort(pickerClips.begin(), pickerClips.end());
}

void editor::AnimatorWindow::drawClipPicker(){
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(360.0f), Theme::dpi(320.0f)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("clip_picker")) return;
    if (pickerStale) refreshPickerClips();
    char filter[128];
    std::snprintf(filter, sizeof(filter), "%s", pickerFilter.c_str());
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##filter", "search clips", filter, sizeof(filter))) pickerFilter = filter;
    ImGui::BeginChild("##clips", ImVec2(0, 0));
    for (const std::string& clip : pickerClips){
        if (!pickerFilter.empty() && clip.find(pickerFilter) == std::string::npos) continue;
        if (ImGui::Selectable(clip.c_str())){
            createState(clip, false, &pickerAt);
            ImGui::CloseCurrentPopup();
        }
    }
    if (pickerClips.empty()) ImGui::TextDisabled("No .anim files or model clips in the project");
    ImGui::EndChild();
    ImGui::EndPopup();
}

void editor::AnimatorWindow::drawInspector(float width, float height){
    ImGui::BeginChild("##animator_inspector", ImVec2(width, height), ImGuiChildFlags_Borders);
    const anim::ControllerLayer* data = currentLayerData();
    if (!data){
        ImGui::EndChild();
        return;
    }
    switch (selected.kind){
        case animator::ItemKind::State:
            if ((size_t)selected.index < data->states.size()){
                if (selectedStateList.size() > 1) ImGui::TextDisabled("%zu states selected", selectedStateList.size());
                else drawStateInspector(selected.index);
            }
            break;
        case animator::ItemKind::Transition:
            if ((size_t)selected.index < data->transitions.size()) drawTransitionInspector(selected.index);
            break;
        case animator::ItemKind::AnyState:
            ImGui::TextUnformatted("Any State");
            ImGui::TextDisabled("Its transitions are checked first, from whatever state the layer is in.");
            break;
        case animator::ItemKind::Parameter:
            if ((size_t)selected.index < doc->data().parameters.size()){
                const anim::ControllerParameter parameter = doc->data().parameters[(size_t)selected.index];
                ImGui::Text("%s (%s)", parameter.name.c_str(), anim::parameterTypeName(parameter.type));
                if (ImGui::SmallButton(ICON_FA_TRASH " Delete Parameter")) removeParameter(selected.index);
            }
            break;
        default:
            ImGui::TextDisabled("Select a state or a transition");
            ImGui::TextDisabled("Right-click the graph to create states;");
            ImGui::TextDisabled("right-click a state to make a transition.");
            break;
    }
    ImGui::EndChild();
}

void editor::AnimatorWindow::drawStateInspector(int state){
    const anim::ControllerData data = doc->data();
    const anim::ControllerState& stateData = data.layers[(size_t)layer].states[(size_t)state];
    ImGui::PushID(state);
    static std::string key;
    auto fieldKey = [&](){
        if (ImGui::IsItemActivated() || key.empty()) key = newMergeKey("state_field");
        return key;
    };
    char name[128];
    std::snprintf(name, sizeof(name), "%s", stateData.name.c_str());
    ImGui::SetNextItemWidth(-Theme::dpi(90.0f));
    ImGui::InputText("Name", name, sizeof(name));
    UIProbe::item("animator.state.name");
    if (ImGui::IsItemDeactivatedAfterEdit()) renameState(state, name);
    if (data.layers[(size_t)layer].defaultStateIndex() == state){
        ImGui::TextColored(ImVec4(0.77f, 0.46f, 0.17f, 1.0f), "Layer default state");
    }else if (ImGui::SmallButton("Set as Layer Default State")){
        setDefaultState(state);
    }

    int motion = stateData.hasBlendTree ? 2 : stateData.clip.empty() ? 0 : 1;
    const char* motions[] = {"None (empty)", "Clip", "Blend Tree"};
    ImGui::SetNextItemWidth(-Theme::dpi(90.0f));
    if (ImGui::Combo("Motion", &motion, motions, 3)){
        editState(state, [&](anim::ControllerState& target){
            if (motion == 2){
                target.clip.clear();
                target.clipGuid.clear();
                target.hasBlendTree = true;
                if (target.blendTree.parameter.empty()){
                    for (const anim::ControllerParameter& parameter : data.parameters){
                        if (parameter.type == anim::ControllerParameterType::Float || parameter.type == anim::ControllerParameterType::Int){
                            target.blendTree.parameter = parameter.name;
                            break;
                        }
                    }
                }
            }else{
                target.hasBlendTree = false;
                target.blendTree = anim::BlendTree1D();
                if (motion == 0){
                    target.clip.clear();
                    target.clipGuid.clear();
                }
            }
        });
    }
    if (!stateData.hasBlendTree){
        char clip[512];
        std::snprintf(clip, sizeof(clip), "%s", stateData.clip.c_str());
        ImGui::SetNextItemWidth(-Theme::dpi(90.0f));
        ImGui::InputTextWithHint("Clip", "drop a clip, or pick", clip, sizeof(clip));
        UIProbe::item("animator.state.clip");
        if (ImGui::IsItemDeactivatedAfterEdit()){
            std::string value, guid;
            if (resolveTypedAsset(clip, true, value, guid)){
                editState(state, [&](anim::ControllerState& target){
                    target.clip = value;
                    target.clipGuid = guid;
                });
            }
        }
        if (ImGui::BeginDragDropTarget()){
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
                const std::vector<std::string> paths = Util::getStringsFromPayload(payload);
                if (!paths.empty() && Util::isAnimationClipFile(paths.front())){
                    const fs::path absolute = fs::u8path(paths.front());
                    const std::string value = project->normalizeToAssetsRelative(absolute).generic_string();
                    const std::string guid = clipGuidOf(value);
                    editState(state, [&](anim::ControllerState& target){
                        target.clip = value;
                        target.clipGuid = guid;
                    });
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    float speed = stateData.speed;
    ImGui::SetNextItemWidth(-Theme::dpi(90.0f));
    if (ImGui::DragFloat("Speed", &speed, 0.01f)){
        const std::string k = fieldKey();
        editState(state, [speed](anim::ControllerState& target){ target.speed = speed; }, k);
    }
    if (ImGui::IsItemDeactivated()) key.clear();
    std::string speedParameter = stateData.speedParameter;
    ImGui::SetNextItemWidth(-Theme::dpi(90.0f));
    if (parameterCombo("Multiplier", data, speedParameter, {anim::ControllerParameterType::Float}, true)){
        editState(state, [speedParameter](anim::ControllerState& target){ target.speedParameter = speedParameter; });
    }
    ImGui::SetItemTooltip("A float parameter the speed is multiplied by");
    int wrap = (int)stateData.wrap;
    const char* wraps[] = {"Default (the clip's)", "Once", "Loop", "Ping Pong", "Clamp Forever"};
    ImGui::SetNextItemWidth(-Theme::dpi(90.0f));
    if (ImGui::Combo("Wrap", &wrap, wraps, 5)){
        editState(state, [wrap](anim::ControllerState& target){ target.wrap = (ClipWrapMode)wrap; });
    }
    float cycle = stateData.cycleOffset;
    ImGui::SetNextItemWidth(-Theme::dpi(90.0f));
    if (ImGui::SliderFloat("Cycle Offset", &cycle, 0.0f, 1.0f)){
        const std::string k = fieldKey();
        editState(state, [cycle](anim::ControllerState& target){ target.cycleOffset = cycle; }, k);
    }
    if (ImGui::IsItemDeactivated()) key.clear();

    // the transitions that leave it, in priority order
    ImGui::SeparatorText("Transitions");
    const anim::ControllerLayer& layerData = data.layers[(size_t)layer];
    bool any = false;
    for (size_t t = 0; t < layerData.transitions.size(); t++){
        if (layerData.transitions[t].from != stateData.name) continue;
        any = true;
        ImGui::PushID((int)t);
        if (ImGui::Selectable((stateData.name + " -> " + layerData.transitions[t].to).c_str())){
            select({animator::ItemKind::Transition, layer, (int)t});
        }
        ImGui::PopID();
    }
    if (!any) ImGui::TextDisabled("none (right-click the state: Make Transition)");
    if (stateData.hasBlendTree) drawBlendTreeInspector(state);
    ImGui::PopID();
}

void editor::AnimatorWindow::drawTransitionInspector(int transition){
    const anim::ControllerData data = doc->data();
    const anim::ControllerLayer& layerData = data.layers[(size_t)layer];
    const anim::ControllerTransition& t = layerData.transitions[(size_t)transition];
    ImGui::PushID(transition);
    static std::string key;
    auto fieldKey = [&](){
        if (ImGui::IsItemActivated() || key.empty()) key = newMergeKey("transition_field");
        return key;
    };
    ImGui::Text("%s -> %s", t.from == anim::ANY_STATE ? "Any State" : t.from.c_str(), t.to.c_str());
    UIProbe::item("animator.transition.title");
    const float labelSpace = Theme::dpi(120.0f);
    bool hasExit = t.hasExitTime;
    if (ImGui::Checkbox("Has Exit Time", &hasExit)){
        editTransition(transition, [hasExit](anim::ControllerTransition& target){ target.hasExitTime = hasExit; });
    }
    UIProbe::item("animator.transition.has_exit_time");
    ImGui::BeginDisabled(!t.hasExitTime);
    float exitTime = t.exitTime;
    ImGui::SetNextItemWidth(-labelSpace);
    if (ImGui::DragFloat("Exit Time", &exitTime, 0.01f, 0.0f, 100.0f)){
        const std::string k = fieldKey();
        editTransition(transition, [exitTime](anim::ControllerTransition& target){ target.exitTime = exitTime; }, k);
    }
    if (ImGui::IsItemDeactivated()) key.clear();
    ImGui::EndDisabled();
    bool fixed = t.fixedDuration;
    if (ImGui::Checkbox("Fixed Duration", &fixed)){
        editTransition(transition, [fixed](anim::ControllerTransition& target){ target.fixedDuration = fixed; });
    }
    float duration = t.duration;
    ImGui::SetNextItemWidth(-labelSpace);
    if (ImGui::DragFloat(t.fixedDuration ? "Duration (s)" : "Duration (%)", &duration, 0.01f, 0.0f, 100.0f)){
        const std::string k = fieldKey();
        editTransition(transition, [duration](anim::ControllerTransition& target){ target.duration = duration; }, k);
    }
    if (ImGui::IsItemDeactivated()) key.clear();
    float offset = t.offset;
    ImGui::SetNextItemWidth(-labelSpace);
    if (ImGui::SliderFloat("Offset", &offset, 0.0f, 1.0f)){
        const std::string k = fieldKey();
        editTransition(transition, [offset](anim::ControllerTransition& target){ target.offset = offset; }, k);
    }
    if (ImGui::IsItemDeactivated()) key.clear();
    int interruption = (int)t.interruption;
    const char* interruptions[] = {"None", "Current State", "Next State"};
    ImGui::SetNextItemWidth(-labelSpace);
    if (ImGui::Combo("Interruption", &interruption, interruptions, 3)){
        editTransition(transition, [interruption](anim::ControllerTransition& target){ target.interruption = (anim::TransitionInterruption)interruption; });
    }
    if (t.from == anim::ANY_STATE){
        bool self = t.canTransitionToSelf;
        if (ImGui::Checkbox("Can Transition To Self", &self)){
            editTransition(transition, [self](anim::ControllerTransition& target){ target.canTransitionToSelf = self; });
        }
    }

    // the timeline: the source on top, the destination below from where the blend starts
    {
        const int from = layerData.findState(t.from);
        const int to = layerData.findState(t.to);
        const float sourceLength = from >= 0 ? motionLength(layerData.states[(size_t)from]) : 1.0f;
        const float destinationLength = to >= 0 ? motionLength(layerData.states[(size_t)to]) : 1.0f;
        const animator::Timeline timeline = animator::transitionTimeline(t, sourceLength, destinationLength,
                                                                         to >= 0 ? layerData.states[(size_t)to].cycleOffset : 0.0f);
        const float total = std::max(timeline.sourceLength, timeline.exitStart + timeline.blend + (timeline.destinationLength - timeline.destinationStart));
        const float barWidth = ImGui::GetContentRegionAvail().x;
        const float rowHeight = Theme::dpi(14.0f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        auto x = [&](float seconds){ return p.x + barWidth * (total > 0.0f ? seconds / total : 0.0f); };
        drawList->AddRectFilled(ImVec2(x(0.0f), p.y), ImVec2(x(timeline.sourceLength), p.y + rowHeight), IM_COL32(80, 80, 90, 255));
        drawList->AddText(ImVec2(x(0.0f) + 3.0f, p.y), IM_COL32(220, 220, 220, 255), t.from == anim::ANY_STATE ? "Any State" : t.from.c_str());
        const float destinationEnd = timeline.exitStart + (timeline.destinationLength - timeline.destinationStart);
        drawList->AddRectFilled(ImVec2(x(timeline.exitStart), p.y + rowHeight + 2.0f), ImVec2(x(destinationEnd), p.y + 2.0f * rowHeight + 2.0f),
                                IM_COL32(70, 90, 120, 255));
        drawList->AddText(ImVec2(x(timeline.exitStart) + 3.0f, p.y + rowHeight + 2.0f), IM_COL32(220, 220, 220, 255), t.to.c_str());
        drawList->AddRectFilled(ImVec2(x(timeline.exitStart), p.y), ImVec2(x(timeline.exitStart + timeline.blend), p.y + 2.0f * rowHeight + 2.0f),
                                IM_COL32(90, 170, 255, 70));
        ImGui::Dummy(ImVec2(barWidth, 2.0f * rowHeight + 4.0f));
        ImGui::SetItemTooltip("The source (top) and the destination (below): the blend starts at %.2f s and lasts %.2f s", timeline.exitStart, timeline.blend);
    }

    // conditions, all must hold
    ImGui::SeparatorText("Conditions");
    for (size_t c = 0; c < t.conditions.size(); c++){
        const anim::ControllerCondition& condition = t.conditions[c];
        ImGui::PushID((int)c);
        std::string parameter = condition.parameter;
        ImGui::SetNextItemWidth(Theme::dpi(90.0f));
        if (parameterCombo("##parameter", data, parameter, {anim::ControllerParameterType::Float, anim::ControllerParameterType::Int,
                                                              anim::ControllerParameterType::Bool, anim::ControllerParameterType::Trigger}, false)){
            const int index = data.findParameter(parameter);
            editTransition(transition, [c, parameter, index, &data](anim::ControllerTransition& target){
                anim::ControllerCondition& edited = target.conditions[c];
                edited.parameter = parameter;
                // a mode that fits the new parameter's type
                const anim::ControllerParameterType type = data.parameters[(size_t)index].type;
                if (!anim::conditionModeFits(edited.mode, type)){
                    edited.mode = type == anim::ControllerParameterType::Float ? anim::ConditionMode::Greater :
                                  type == anim::ControllerParameterType::Int ? anim::ConditionMode::Equals : anim::ConditionMode::If;
                }
            });
        }
        ImGui::SameLine();
        const int parameterIndex = data.findParameter(condition.parameter);
        ImGui::SetNextItemWidth(Theme::dpi(80.0f));
        if (ImGui::BeginCombo("##mode", anim::conditionModeName(condition.mode))){
            for (int m = 0; m < 6; m++){
                const anim::ConditionMode mode = (anim::ConditionMode)m;
                if (parameterIndex >= 0 && !anim::conditionModeFits(mode, data.parameters[(size_t)parameterIndex].type)) continue;
                if (ImGui::Selectable(anim::conditionModeName(mode), mode == condition.mode)){
                    editTransition(transition, [c, mode](anim::ControllerTransition& target){ target.conditions[c].mode = mode; });
                }
            }
            ImGui::EndCombo();
        }
        if (condition.mode != anim::ConditionMode::If && condition.mode != anim::ConditionMode::IfNot){
            ImGui::SameLine();
            float threshold = condition.threshold;
            ImGui::SetNextItemWidth(Theme::dpi(60.0f));
            if (ImGui::DragFloat("##threshold", &threshold, 0.01f)){
                const std::string k = fieldKey();
                editTransition(transition, [c, threshold](anim::ControllerTransition& target){ target.conditions[c].threshold = threshold; }, k);
            }
            if (ImGui::IsItemDeactivated()) key.clear();
        }
        ImGui::SameLine();
        if (c > 0 && ImGui::SmallButton(ICON_FA_ARROW_UP)){
            editTransition(transition, [c](anim::ControllerTransition& target){ std::swap(target.conditions[c], target.conditions[c - 1]); });
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_XMARK)){
            editTransition(transition, [c](anim::ControllerTransition& target){ target.conditions.erase(target.conditions.begin() + (long)c); });
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton(ICON_FA_PLUS " Condition")){
        if (data.parameters.empty()){
            setNotice("Add a parameter first (Parameters tab)", true);
        }else{
            const anim::ControllerParameter first = data.parameters.front();
            editTransition(transition, [first](anim::ControllerTransition& target){
                anim::ControllerCondition condition;
                condition.parameter = first.name;
                condition.mode = first.type == anim::ControllerParameterType::Float ? anim::ConditionMode::Greater :
                                 first.type == anim::ControllerParameterType::Int ? anim::ConditionMode::Equals : anim::ConditionMode::If;
                target.conditions.push_back(condition);
            });
        }
    }
    UIProbe::item("animator.transition.add_condition");
    if (t.conditions.empty() && !t.hasExitTime) ImGui::TextColored(Theme::Colors::WarningText, "Neither conditions nor an exit time: it fires at once");
    ImGui::Spacing();
    if (ImGui::SmallButton(ICON_FA_TRASH " Delete Transition")){
        selected = {animator::ItemKind::Transition, layer, transition};
        deleteSelection();
    }
    ImGui::PopID();
}

void editor::AnimatorWindow::drawBlendTreeInspector(int state){
    const anim::ControllerData data = doc->data();
    const anim::ControllerState& stateData = data.layers[(size_t)layer].states[(size_t)state];
    const anim::BlendTree1D& tree = stateData.blendTree;
    static std::string key;
    ImGui::SeparatorText("Blend Tree (1D)");
    std::string parameter = tree.parameter;
    ImGui::SetNextItemWidth(-Theme::dpi(90.0f));
    if (parameterCombo("Parameter", data, parameter, {anim::ControllerParameterType::Float, anim::ControllerParameterType::Int}, false)){
        editState(state, [parameter](anim::ControllerState& target){ target.blendTree.parameter = parameter; });
    }
    bool automate = tree.automateThresholds;
    if (ImGui::Checkbox("Automate Thresholds", &automate)){
        editState(state, [automate](anim::ControllerState& target){ target.blendTree.automateThresholds = automate; });
    }
    for (size_t c = 0; c < tree.children.size(); c++){
        const anim::BlendTreeChild& child = tree.children[c];
        ImGui::PushID((int)c);
        char clip[512];
        std::snprintf(clip, sizeof(clip), "%s", child.clip.c_str());
        ImGui::SetNextItemWidth(Theme::dpi(120.0f));
        ImGui::InputTextWithHint("##clip", "clip", clip, sizeof(clip));
        if (ImGui::IsItemDeactivatedAfterEdit()){
            std::string value, guid;
            if (resolveTypedAsset(clip, true, value, guid)){
                editState(state, [c, value, guid](anim::ControllerState& target){
                    target.blendTree.children[c].clip = value;
                    target.blendTree.children[c].clipGuid = guid;
                });
            }
        }
        if (ImGui::BeginDragDropTarget()){
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
                const std::vector<std::string> paths = Util::getStringsFromPayload(payload);
                if (!paths.empty() && Util::isAnimationClipFile(paths.front())){
                    const fs::path absolute = fs::u8path(paths.front());
                    const std::string value = project->normalizeToAssetsRelative(absolute).generic_string();
                    const std::string guid = clipGuidOf(value);
                    editState(state, [c, value, guid](anim::ControllerState& target){
                        target.blendTree.children[c].clip = value;
                        target.blendTree.children[c].clipGuid = guid;
                    });
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(tree.automateThresholds);
        // dragged as a local value and committed on release: a commit re-sorts the children by
        // threshold, which would move the dragged one under the cursor's index mid-drag
        const bool dragged = thresholdDragState == state && thresholdDragChild == (int)c;
        float threshold = dragged ? thresholdDragValue : child.threshold;
        ImGui::SetNextItemWidth(Theme::dpi(50.0f));
        ImGui::DragFloat("##threshold", &threshold, 0.01f);
        if (ImGui::IsItemActive()){
            thresholdDragState = state;
            thresholdDragChild = (int)c;
            thresholdDragValue = threshold;
        }
        if (ImGui::IsItemDeactivated()){
            const bool edited = ImGui::IsItemDeactivatedAfterEdit();
            thresholdDragState = thresholdDragChild = -1;
            if (edited) setBlendThreshold(state, (int)c, threshold);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        float speed = child.speed;
        ImGui::SetNextItemWidth(Theme::dpi(40.0f));
        if (ImGui::DragFloat("##speed", &speed, 0.01f)){
            if (ImGui::IsItemActivated() || key.empty()) key = newMergeKey("tree_speed");
            editState(state, [c, speed](anim::ControllerState& target){ target.blendTree.children[c].speed = speed; }, key);
        }
        ImGui::SetItemTooltip("Speed");
        if (ImGui::IsItemDeactivated()) key.clear();
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_XMARK)){
            editState(state, [c](anim::ControllerState& target){ target.blendTree.children.erase(target.blendTree.children.begin() + (long)c); });
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton(ICON_FA_PLUS " Child")){
        editState(state, [](anim::ControllerState& target){
            anim::BlendTreeChild child;
            child.threshold = target.blendTree.children.empty() ? 0.0f : target.blendTree.children.back().threshold + 1.0f;
            target.blendTree.children.push_back(child);
        });
    }
    UIProbe::item("animator.tree.add_child");

    // the weight curves over the parameter's range, and the scrubbed value
    if (!tree.children.empty()){
        float lo = tree.children.front().threshold, hi = tree.children.back().threshold;
        if (hi <= lo) hi = lo + 1.0f;
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = Theme::dpi(50.0f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(p, ImVec2(p.x + width, p.y + height), IM_COL32(35, 35, 38, 255));
        std::vector<float> thresholds;
        for (const anim::BlendTreeChild& child : tree.children) thresholds.push_back(child.threshold);
        const ImU32 colors[] = {IM_COL32(90, 170, 255, 255), IM_COL32(240, 160, 60, 255), IM_COL32(120, 210, 120, 255), IM_COL32(220, 100, 180, 255)};
        std::vector<float> weights;
        const int samples = 48;
        for (size_t c = 0; c < tree.children.size(); c++){
            ImVec2 previous;
            for (int i = 0; i <= samples; i++){
                const float value = lo + (hi - lo) * (float)i / (float)samples;
                anim::blendTreeWeights(thresholds, value, weights);
                const ImVec2 point(p.x + width * (float)i / (float)samples, p.y + height - 2.0f - (height - 4.0f) * weights[c]);
                if (i > 0) drawList->AddLine(previous, point, colors[c % 4], 1.5f);
                previous = point;
            }
        }
        const float marker = p.x + width * std::min(1.0f, std::max(0.0f, (blendValue - lo) / (hi - lo)));
        drawList->AddLine(ImVec2(marker, p.y), ImVec2(marker, p.y + height), IM_COL32(255, 255, 255, 200), 1.5f);
        ImGui::Dummy(ImVec2(width, height));
        ImGui::SetNextItemWidth(-Theme::dpi(90.0f));
        ImGui::SliderFloat("Preview", &blendValue, lo, hi);
        UIProbe::item("animator.tree.value");
        ImGui::SetItemTooltip("The parameter value the Scene view preview shows (Preview in the header)");
    }
}

void editor::AnimatorWindow::drawValidationStrip(){
    ImGui::BeginChild("##animator_warnings", ImVec2(0, 0), ImGuiChildFlags_Borders);
    const std::vector<ClipWarning> list = warnings();
    for (size_t i = 0; i < list.size(); i++){
        ImGui::PushID((int)i);
        const std::string label = std::string(ICON_FA_TRIANGLE_EXCLAMATION " ") + (list[i].path.empty() ? "" : list[i].path + ": ") + list[i].message;
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
        if (ImGui::Selectable(label.c_str())) selectPath(list[i].path);
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::EndChild();
}
