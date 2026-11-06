// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimatorWindow.h"

#include "shortcuts/CommandCatalog.h"

#include "App.h"
#include "Backend.h"
#include "Project.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimatorEditorHooks.h"
#include "animation/AnimatorInstance.h"
#include "animation/BlendTree.h"
#include "animation/CompiledController.h"
#include "animation/ModelSubClips.h"
#include "component/AnimatorComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/Transform.h"
#include "external/IconsFontAwesome6.h"
#include "pool/AnimationClipPool.h"
#include "subsystem/AnimatorSystem.h"
#include "subsystem/ParticleSubsystem.h"
#include "util/FileDialogs.h"
#include "util/ProjectChecks.h"
#include "util/UIProbe.h"
#include "util/Util.h"
#include "window/PoseAnimationWindow.h"
#include "AppSettings.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::anim;
namespace fs = std::filesystem;

namespace{

    constexpr uint64_t kEntry = 1, kAny = 2, kExit = 3, kUp = 4;
    constexpr uint64_t kStateBase = 0x1000, kMachineBase = 0x100000;
    constexpr uint64_t kListState = 1, kListAny = 2, kListEntry = 3, kListExit = 4, kListDefault = 5;

    uint64_t linkId(uint64_t list, uint64_t owner, uint64_t index){
        return (list << 40) | (owner << 20) | index;
    }

    const ImU32 kFillState = IM_COL32(72, 72, 78, 255);
    const ImU32 kFillDefault = IM_COL32(200, 115, 35, 255);
    const ImU32 kFillMachine = IM_COL32(55, 78, 112, 255);
    const ImU32 kFillEntry = IM_COL32(40, 135, 70, 255);
    const ImU32 kFillAny = IM_COL32(35, 145, 165, 255);
    const ImU32 kFillExit = IM_COL32(165, 50, 50, 255);
    const ImU32 kFillUp = IM_COL32(85, 85, 85, 255);

    double nowSeconds(){
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    std::string uniqueName(const StateMachineDef& machine, const std::string& wanted){
        auto taken = [&](const std::string& name){
            for (const StateDef& s : machine.states) if (s.name == name) return true;
            for (const StateMachineDef& m : machine.subMachines) if (m.name == name) return true;
            return false;
        };
        if (!taken(wanted)) return wanted;
        for (int n = 1;; n++){
            const std::string candidate = wanted + " " + std::to_string(n);
            if (!taken(candidate)) return candidate;
        }
    }

    std::string motionLabel(const MotionDef& motion){
        if (motion.isTree()) return std::string("Blend Tree (") + blendTreeTypeName(motion.tree.front().type) + ")";
        if (motion.clip.empty()) return std::string();
        SubClipReference subClip;
        if (parseSubClipReference(motion.clip.path, subClip)) return subClip.clip;
        return fs::path(motion.clip.path).stem().string();
    }

    bool comboEnum(const char* label, int& value, const std::vector<const char*>& names){
        bool changed = false;
        if (ImGui::BeginCombo(label, value >= 0 && (size_t)value < names.size() ? names[(size_t)value] : "")){
            for (size_t i = 0; i < names.size(); i++){
                if (ImGui::Selectable(names[i], (int)i == value)){
                    value = (int)i;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    // a parameter of `def` by name (empty: none), optionally of the number types only
    bool comboParameter(const char* label, const ControllerDef& def, std::string& value, bool numbersOnly){
        bool changed = false;
        if (ImGui::BeginCombo(label, value.empty() ? "(none)" : value.c_str())){
            if (ImGui::Selectable("(none)", value.empty())){
                value.clear();
                changed = true;
            }
            for (const ParameterDef& p : def.parameters){
                if (numbersOnly && p.type != AnimatorParameterType::Float && p.type != AnimatorParameterType::Int) continue;
                if (ImGui::Selectable(p.name.c_str(), p.name == value)){
                    value = p.name;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    // the assets-relative clip reference of a dropped or picked file ("<model>#<clip>" kept)
    bool clipFromPath(Project* project, const std::string& path, AnimatorAssetRef& out){
        SubClipReference subClip;
        const bool isSubClip = parseSubClipReference(path, subClip);
        const std::string file = isSubClip ? subClip.model : path;
        if (!project->isInsideAssetsPath(file) && !project->isInsideAssetsPath(project->resolveAssetPath(file).string())) return false;
        const fs::path absolute = project->isInsideAssetsPath(file) ? fs::path(file) : project->resolveAssetPath(file);
        const std::string stored = project->normalizeToAssetsRelative(absolute).generic_string();
        if (isSubClip){
            out.path = formatSubClipReference(stored, subClip.clip);
            out.guid.clear();
            return true;
        }
        if (!Util::isAnimationClipFile(stored)) return false;
        out.path = stored;
        std::string text;
        out.guid = ProjectChecks::readFile(absolute, text) ? readClipGuid(text) : std::string();
        return true;
    }

    // every transition list of a machine and its sub-machines
    void forEachTransition(StateMachineDef& machine, const std::function<void(TransitionDef&)>& visit){
        for (TransitionDef& t : machine.entry) visit(t);
        for (TransitionDef& t : machine.anyState) visit(t);
        for (TransitionDef& t : machine.exitTransitions) visit(t);
        for (StateDef& s : machine.states) for (TransitionDef& t : s.transitions) visit(t);
        for (StateMachineDef& sub : machine.subMachines) forEachTransition(sub, visit);
    }

    void forEachState(StateMachineDef& machine, const std::function<void(StateDef&)>& visit){
        for (StateDef& s : machine.states) visit(s);
        for (StateMachineDef& sub : machine.subMachines) forEachState(sub, visit);
    }

    void forEachTree(MotionDef& motion, const std::function<void(BlendTreeDef&)>& visit){
        if (!motion.isTree()) return;
        visit(motion.tree.front());
        for (BlendChildDef& child : motion.tree.front().children) forEachTree(child.motion, visit);
    }

    // "<list>[<n>]" at the start of `rest`: n, and `rest` past it (and a following '.')
    bool takeIndexed(std::string& rest, const std::string& list, int& index){
        const std::string head = list + "[";
        if (rest.compare(0, head.size(), head) != 0) return false;
        const size_t close = rest.find(']', head.size());
        if (close == std::string::npos) return false;
        try{
            index = std::stoi(rest.substr(head.size(), close - head.size()));
        }catch (...){
            return false;
        }
        rest = rest.substr(close + 1);
        if (!rest.empty() && rest[0] == '.') rest = rest.substr(1);
        return index >= 0;
    }

}

AnimatorWindow::AnimatorWindow(Project* project, PoseAnimationWindow*): project(project){
    animatorEditorHooks().openController = [this](Project*, uint32_t scene, Entity target, const std::string& path){
        openController(path, scene, target);
        setOpen(true);
        requestFocus();
    };
    // the Avatar Configuration window (upstream's anim-retarget: bones, fingers, eyes and jaw,
    // twist corrections, a retargeted test clip), the one the Model Inspector's Rig foldout opens
    animatorEditorHooks().configureAvatar = [](Project*, uint32_t scene, Entity model){
        Backend::getApp().openAvatarWindow(scene, model);
    };
    animatorEditorHooks().drawSelection = [this](){ return drawSelectionInspector(); };
}

AnimatorWindow::~AnimatorWindow(){
    stopPreview();
    animatorEditorHooks().openController = nullptr;
    animatorEditorHooks().configureAvatar = nullptr;
    animatorEditorHooks().drawSelection = nullptr;
}

void AnimatorWindow::setOpen(bool open){
    windowOpen = open;
    if (!open) stopPreview();
}

void AnimatorWindow::requestFocus(){
    focusRequest = true;
}

void AnimatorWindow::clearSceneState(uint32_t closed){
    if (previewScene == closed) stopPreview();
    if (sceneId == closed){
        sceneId = 0;
        entity = NULL_ENTITY;
        pinned = false;
    }
}

void AnimatorWindow::prepareForProjectSwitch(){
    stopPreview();
    pending.reset();
    document.reset();
    sceneId = 0;
    entity = NULL_ENTITY;
    pinned = false;
    selection = Selection();
    machinePath.clear();
    layer = 0;
    inspectorClaim = false;
    // the documents belong to the project that closes
    AnimatorAssetDocument::closeAll();
}

void AnimatorWindow::openController(const std::string& path, uint32_t scene, Entity target){
    setDocument(path);
    sceneId = scene;
    entity = target;
    pinned = target != NULL_ENTITY;
    setCurrentLayer(0);
    selection = Selection();
}

void AnimatorWindow::setDocument(const std::string& path){
    if (path.empty()){
        pending.reset();
        document.reset();
        return;
    }
    if (document && (document->assetPath() == path || document->projectPath() == path)) return;
    flush();
    pending.reset();
    std::string error;
    document = AnimatorAssetDocument::open(project, path, &error);
    if (!document) status = error;
    documentRevision = document ? document->revision() : 0;
    setCurrentLayer(0);
    selection = Selection();
    canvas.frame({});
}

void AnimatorWindow::setCurrentLayer(int index){
    layer = index;
    machinePath.clear();
    const ControllerDef def = working();
    layerName = index >= 0 && (size_t)index < def.layers.size() ? def.layers[(size_t)index].name : std::string();
}

void AnimatorWindow::setLayer(int index){
    syncToDocument();
    setCurrentLayer(index);
    selection = Selection();
    selection.kind = SelectionKind::Layer;
    canvas.frame({});
}

int AnimatorWindow::getLayer(){
    syncToDocument();
    return layer;
}

bool AnimatorWindow::selectionExists(const ControllerDef& def){
    ControllerDef copy = def;
    StateMachineDef* machine = machineOf(copy);
    if (!machine) return false;
    switch (selection.kind){
        case SelectionKind::None:
        case SelectionKind::Layer:
            return true;
        case SelectionKind::State:
            for (const StateDef& st : machine->states) if (st.name == selection.name) return true;
            return false;
        case SelectionKind::SubMachine:
            for (const StateMachineDef& m : machine->subMachines) if (m.name == selection.name) return true;
            return false;
        case SelectionKind::Transition: {
            const std::vector<TransitionDef>* list = nullptr;
            if (selection.list == ListKind::AnyState) list = &machine->anyState;
            else if (selection.list == ListKind::Entry) list = &machine->entry;
            else if (selection.list == ListKind::Exit){
                for (const StateMachineDef& sub : machine->subMachines) if (sub.name == selection.owner) list = &sub.exitTransitions;
            }else{
                for (const StateDef& st : machine->states) if (st.name == selection.owner) list = &st.transitions;
            }
            return list && selection.index >= 0 && (size_t)selection.index < list->size();
        }
    }
    return false;
}

void AnimatorWindow::syncToDocument(){
    if (!document || document->revision() == documentRevision) return;
    documentRevision = document->revision();
    const ControllerDef& def = document->controller();
    int found = -1;
    for (size_t l = 0; l < def.layers.size(); l++) if (def.layers[l].name == layerName) found = (int)l;
    if (found < 0){
        // the layer is gone (or renamed outside): the base, nothing selected
        setCurrentLayer(0);
        selection = Selection();
        canvas.frame({});
        return;
    }
    if (found != layer){
        // the same layer at another index: the old indices' selection goes
        layer = found;
        selection = Selection();
        return;
    }
    if (!selectionExists(def)){
        selection = Selection();
        return;
    }
    // the rest of a multi-selection without what is gone
    if (const StateMachineDef* machine = machineOf(const_cast<ControllerDef&>(def))){
        auto gone = [machine](const std::string& name){
            if (name == "Entry" || name == "Any State" || name == "Exit") return false;
            for (const StateDef& st : machine->states) if (st.name == name) return false;
            for (const StateMachineDef& m : machine->subMachines) if (m.name == name) return false;
            return true;
        };
        selection.others.erase(std::remove_if(selection.others.begin(), selection.others.end(), gone), selection.others.end());
    }
}

bool AnimatorWindow::isNodeSelected(const std::string& name) const{
    if ((selection.kind == SelectionKind::State || selection.kind == SelectionKind::SubMachine) && selection.name == name) return true;
    return std::find(selection.others.begin(), selection.others.end(), name) != selection.others.end();
}

std::vector<std::string> AnimatorWindow::selectedNodes(){
    syncToDocument();
    std::vector<std::string> names;
    if (selection.kind == SelectionKind::State || selection.kind == SelectionKind::SubMachine) names.push_back(selection.name);
    names.insert(names.end(), selection.others.begin(), selection.others.end());
    return names;
}

void AnimatorWindow::followSelection(){
    if (pinned){
        SceneProject* scene = project->getScene(sceneId);
        if (!scene || !scene->scene || !scene->scene->isEntityCreated(entity) || !scene->scene->findComponent<AnimatorComponent>(entity)){
            pinned = false;
        }else{
            setDocument(scene->scene->getComponent<AnimatorComponent>(entity).controller.path);
            return;
        }
    }
    SceneProject* scene = project->getSelectedScene();
    if (!scene || !scene->scene) return;
    const std::vector<Entity> selected = project->getSelectedEntities(scene->id);
    if (selected.size() != 1) return;
    AnimatorComponent* animator = scene->scene->findComponent<AnimatorComponent>(selected[0]);
    if (!animator) return;
    if (scene->id != sceneId || selected[0] != entity){
        if (previewToken != 0) stopPreview();
        sceneId = scene->id;
        entity = selected[0];
    }
    if (!animator->controller.path.empty()) setDocument(animator->controller.path);
}

ControllerDef AnimatorWindow::working() const{
    if (pending) return *pending;
    return document ? document->controller() : ControllerDef();
}

void AnimatorWindow::stage(const ControllerDef& def, const std::string& undoName){
    pending = def;
    pendingUndo = undoName;
}

void AnimatorWindow::flush(){
    if (!pending || ImGui::IsAnyItemActive()) return;
    const ControllerDef def = *pending;
    const std::string undo = pendingUndo;
    pending.reset();
    commit(def, undo);
}

bool AnimatorWindow::editable() const{
    return document && document->loadError().empty();
}

bool AnimatorWindow::commit(const ControllerDef& def, const std::string& undoName, const std::string& mergeKey){
    pending.reset();
    if (!document) return false;
    std::string error;
    if (!document->commit(def, undoName, mergeKey, &error)){
        status = error;
        return false;
    }
    documentRevision = document->revision();
    status.clear();
    return true;
}

StateMachineDef* AnimatorWindow::machineOf(ControllerDef& def){
    if (layer < 0 || (size_t)layer >= def.layers.size()) return nullptr;
    StateMachineDef* machine = &def.layers[(size_t)layer].machine;
    for (const std::string& name : machinePath){
        StateMachineDef* next = nullptr;
        for (StateMachineDef& sub : machine->subMachines) if (sub.name == name) next = &sub;
        if (!next) return machine;
        machine = next;
    }
    return machine;
}

anim::AnimatorInstance* AnimatorWindow::liveInstance() const{
    SceneProject* scene = project->getScene(sceneId);
    if (!scene || !scene->scene || entity == NULL_ENTITY || !scene->scene->isEntityCreated(entity)) return nullptr;
    const bool running = scene->playState != ScenePlayState::STOPPED || (previewToken != 0 && previewEntity == entity);
    if (!running) return nullptr;
    AnimatorComponent* animator = scene->scene->findComponent<AnimatorComponent>(entity);
    if (!animator || !animator->runtime || !animator->runtime->controller || !animator->runtime->bound) return nullptr;
    return animator->runtime.get();
}

void AnimatorWindow::claimInspector(){
    inspectorClaim = true;
    claimScene = project->getSelectedSceneId();
    claimEntities = project->getSelectedEntities(claimScene);
    claimAsset = project->getInspectedAsset().string();
}

bool AnimatorWindow::claimHolds(){
    if (!inspectorClaim) return false;
    const uint32_t scene = project->getSelectedSceneId();
    if (scene != claimScene || project->getSelectedEntities(scene) != claimEntities || project->getInspectedAsset().string() != claimAsset){
        inspectorClaim = false;
    }
    return inspectorClaim;
}

bool AnimatorWindow::inspectorShowsSelection() const{
    return inspectorFrame >= ImGui::GetFrameCount() - 1;
}

// ---------------------------------------------------------------- edits

bool AnimatorWindow::addState(const std::string& name, const ImVec2& position, const MotionDef& motion){
    ControllerDef def = working();
    StateMachineDef* machine = machineOf(def);
    if (!machine) return false;
    StateDef state;
    state.name = uniqueName(*machine, name.empty() ? std::string("New State") : name);
    state.position = Vector2(position.x, position.y);
    state.motion = motion;
    const bool first = machine->states.empty();
    machine->states.push_back(state);
    if (first && machine->defaultState.empty()) machine->defaultState = state.name;
    if (!commit(def, "Create State " + state.name)) return false;
    selectState(state.name);
    return true;
}

bool AnimatorWindow::addSubStateMachine(const std::string& name, const ImVec2& position){
    ControllerDef def = working();
    StateMachineDef* machine = machineOf(def);
    if (!machine) return false;
    StateMachineDef sub;
    sub.name = uniqueName(*machine, name.empty() ? std::string("New StateMachine") : name);
    sub.position = Vector2(position.x, position.y);
    machine->subMachines.push_back(sub);
    if (!commit(def, "Create Sub-State Machine " + sub.name)) return false;
    selection = Selection();
    selection.kind = SelectionKind::SubMachine;
    selection.name = sub.name;
    return true;
}

bool AnimatorWindow::addTransition(const std::string& fromState, const std::string& toNode, bool fromAnyState, bool fromEntry){
    ControllerDef def = working();
    StateMachineDef* machine = machineOf(def);
    if (!machine) return false;
    TransitionDef t;
    t.to = toNode;
    std::vector<TransitionDef>* list = nullptr;
    Selection made;
    made.kind = SelectionKind::Transition;
    if (fromAnyState){
        list = &machine->anyState;
        made.list = ListKind::AnyState;
        t.hasExitTime = false;
        t.duration = 0.1f;
    }else if (fromEntry){
        list = &machine->entry;
        made.list = ListKind::Entry;
        t.duration = 0.0f;
    }else{
        for (StateDef& s : machine->states) if (s.name == fromState) list = &s.transitions;
        if (!list){
            for (StateMachineDef& sub : machine->subMachines){
                if (sub.name == fromState){
                    list = &sub.exitTransitions;
                    made.list = ListKind::Exit;
                }
            }
        }else{
            made.list = ListKind::State;
            // Unity's defaults for a new transition: exit time 0.75, 0.25 s
            t.hasExitTime = true;
            t.exitTime = 0.75f;
            t.duration = 0.25f;
        }
        made.owner = fromState;
    }
    if (!list) return false;
    list->push_back(t);
    made.index = (int)list->size() - 1;
    if (!commit(def, "Make Transition " + (fromAnyState ? std::string("Any State") : (fromEntry ? std::string("Entry") : fromState)) + " -> " + toNode)) return false;
    selection = made;
    return true;
}

bool AnimatorWindow::setDefaultState(const std::string& name){
    ControllerDef def = working();
    StateMachineDef* machine = machineOf(def);
    if (!machine) return false;
    machine->defaultState = name;
    return commit(def, "Set Default State " + name);
}

bool AnimatorWindow::canDelete(){
    syncToDocument();
    return windowOpen && windowFocused && editable() && selection.kind != SelectionKind::None && selection.kind != SelectionKind::Layer;
}

bool AnimatorWindow::removeSelection(){
    syncToDocument();
    ControllerDef def = working();
    StateMachineDef* machine = machineOf(def);
    if (!machine) return false;
    bool removed = false;
    std::string what;
    // the nodes going: the Inspector's and the rest of a multi-selection, in one step
    std::vector<std::string> gone;
    if (selection.kind == SelectionKind::State || selection.kind == SelectionKind::SubMachine){
        std::vector<std::string> selected = {selection.name};
        selected.insert(selected.end(), selection.others.begin(), selection.others.end());
        // (only what is erased: a selected Exit stays, and with it the transitions to it)
        for (const std::string& name : selected){
            for (size_t i = 0; i < machine->states.size(); i++){
                if (machine->states[i].name != name) continue;
                machine->states.erase(machine->states.begin() + (long)i);
                gone.push_back(name);
                break;
            }
            for (size_t i = 0; i < machine->subMachines.size(); i++){
                if (machine->subMachines[i].name != name) continue;
                machine->subMachines.erase(machine->subMachines.begin() + (long)i);
                gone.push_back(name);
                break;
            }
        }
        const size_t count = gone.size();
        removed = count > 0;
        if (std::find(gone.begin(), gone.end(), machine->defaultState) != gone.end()){
            machine->defaultState = machine->states.empty() ? std::string() : machine->states.front().name;
        }
        if (count > 1) what = "Delete " + std::to_string(count) + " Nodes";
        else what = std::string(selection.kind == SelectionKind::State ? "Delete State " : "Delete Sub-State Machine ") + selection.name;
    }else if (selection.kind == SelectionKind::Transition){
        std::vector<TransitionDef>* list = nullptr;
        if (selection.list == ListKind::AnyState) list = &machine->anyState;
        else if (selection.list == ListKind::Entry) list = &machine->entry;
        else if (selection.list == ListKind::Exit){
            for (StateMachineDef& sub : machine->subMachines) if (sub.name == selection.owner) list = &sub.exitTransitions;
        }else{
            for (StateDef& s : machine->states) if (s.name == selection.owner) list = &s.transitions;
        }
        if (list && selection.index >= 0 && (size_t)selection.index < list->size()){
            list->erase(list->begin() + selection.index);
            removed = true;
            what = "Delete Transition";
        }
    }
    if (!removed) return false;
    if (!gone.empty()){
        // the transitions into them (or into a sub-machine's states) go with them
        auto into = [&](const TransitionDef& t){
            for (const std::string& name : gone) if (t.to == name || t.to.rfind(name + "/", 0) == 0) return true;
            return false;
        };
        auto prune = [&](std::vector<TransitionDef>& list){ list.erase(std::remove_if(list.begin(), list.end(), into), list.end()); };
        prune(machine->entry);
        prune(machine->anyState);
        for (StateDef& s : machine->states) prune(s.transitions);
        for (StateMachineDef& sub : machine->subMachines) prune(sub.exitTransitions);
    }
    selection = Selection();
    return commit(def, what);
}

bool AnimatorWindow::addParameter(const std::string& name, AnimatorParameterType type){
    ControllerDef def = working();
    ParameterDef p;
    p.type = type;
    std::string wanted = name;
    int n = 0;
    while (def.findParameter(wanted) >= 0) wanted = name + " " + std::to_string(++n);
    p.name = wanted;
    def.parameters.push_back(p);
    return commit(def, "Add Parameter " + wanted);
}

bool AnimatorWindow::addLayer(const std::string& name){
    ControllerDef def = working();
    LayerDef l;
    std::string wanted = name;
    int n = 0;
    while (def.findLayer(wanted) >= 0) wanted = name + " " + std::to_string(++n);
    l.name = wanted;
    def.layers.push_back(l);
    if (!commit(def, "Add Layer " + wanted)) return false;
    setCurrentLayer((int)def.layers.size() - 1);
    selection = Selection();
    return true;
}

void AnimatorWindow::selectState(const std::string& name){
    syncToDocument();
    selection = Selection();
    selection.kind = SelectionKind::State;
    selection.name = name;
}

void AnimatorWindow::selectTransition(const std::string& fromState, int index, bool fromAnyState, bool fromEntry){
    syncToDocument();
    selection = Selection();
    selection.kind = SelectionKind::Transition;
    selection.index = index;
    if (fromAnyState) selection.list = ListKind::AnyState;
    else if (fromEntry) selection.list = ListKind::Entry;
    else{
        selection.list = ListKind::State;
        selection.owner = fromState;
        ControllerDef def = working();
        if (StateMachineDef* machine = machineOf(def)){
            for (const StateMachineDef& sub : machine->subMachines) if (sub.name == fromState) selection.list = ListKind::Exit;
        }
    }
}

std::string AnimatorWindow::describeSelection(){
    syncToDocument();
    switch (selection.kind){
        case SelectionKind::None: return std::string();
        case SelectionKind::Layer: return "layer";
        case SelectionKind::State: return "state:" + selection.name;
        case SelectionKind::SubMachine: return "machine:" + selection.name;
        case SelectionKind::Transition: break;
    }
    const char* list = selection.list == ListKind::AnyState ? "any_state" : selection.list == ListKind::Entry ? "entry" :
                       selection.list == ListKind::Exit ? "exit" : "state";
    return std::string("transition:") + list + ":" + selection.owner + ":" + std::to_string(selection.index);
}

bool AnimatorWindow::renameState(const std::string& from, const std::string& to){
    syncToDocument();
    if (to.empty() || from == to) return false;
    ControllerDef def = working();
    StateMachineDef* machine = machineOf(def);
    if (!machine) return false;
    StateDef* state = nullptr;
    for (StateDef& st : machine->states) if (st.name == from) state = &st;
    if (!state){
        status = "no state \"" + from + "\" in this machine";
        return false;
    }
    std::string wanted = to;
    for (char& c : wanted) if (c == '/' || c == '.') c = '_';
    if (wanted == "Entry" || wanted == "Exit" || wanted == "Any State"){
        status = "\"" + wanted + "\" is reserved";
        return false;
    }
    const std::string unique = uniqueName(*machine, wanted);
    state->name = unique;
    // this machine's transitions naming it, and its default
    auto fix = [&](std::vector<TransitionDef>& list){ for (TransitionDef& t : list) if (t.to == from) t.to = unique; };
    fix(machine->entry);
    fix(machine->anyState);
    for (StateDef& st : machine->states) fix(st.transitions);
    if (machine->defaultState == from) machine->defaultState = unique;
    if (!commit(def, "Rename State " + from + " -> " + unique)) return false;
    if (selection.kind == SelectionKind::State && selection.name == from) selection.name = unique;
    std::replace(selection.others.begin(), selection.others.end(), from, unique);
    if (selection.kind == SelectionKind::Transition && selection.owner == from) selection.owner = unique;
    return true;
}

bool AnimatorWindow::renameParameter(const std::string& from, const std::string& to){
    syncToDocument();
    ControllerDef def = working();
    const int index = def.findParameter(from);
    if (index < 0){
        status = "no parameter \"" + from + "\"";
        return false;
    }
    if (to.empty()){
        status = "a parameter needs a name";
        return false;
    }
    if (to == from) return false;
    if (def.findParameter(to) >= 0){
        status = "a parameter \"" + to + "\" exists";
        return false;
    }
    def.parameters[(size_t)index].name = to;
    for (LayerDef& l : def.layers){
        forEachTransition(l.machine, [&](TransitionDef& t){
            for (ConditionDef& c : t.conditions) if (c.parameter == from) c.parameter = to;
        });
        forEachState(l.machine, [&](StateDef& st){
            for (std::string* sp : {&st.speedParameter, &st.timeParameter, &st.cycleOffsetParameter}) if (*sp == from) *sp = to;
            forEachTree(st.motion, [&](BlendTreeDef& tree){
                if (tree.parameter == from) tree.parameter = to;
                if (tree.parameterY == from) tree.parameterY = to;
                for (BlendChildDef& c : tree.children) if (c.directParameter == from) c.directParameter = to;
            });
        });
    }
    if (!commit(def, "Rename Parameter " + from + " -> " + to)) return false;
    // (S21 keeps any name; Lua reads it as a string: a name that is not a Lua identifier is a
    // validation warning, not a refusal)
    if (!isLuaIdentifier(to)) status = "\"" + to + "\" is not a Lua identifier (scripts pass it as a string)";
    return true;
}

bool AnimatorWindow::removeParameter(const std::string& name){
    syncToDocument();
    ControllerDef def = working();
    const int index = def.findParameter(name);
    if (index < 0) return false;
    def.parameters.erase(def.parameters.begin() + index);
    // the conditions that read it go with it, the trees and state parameters reading it are cleared
    removeParameterReferences(def, name);
    return commit(def, "Delete Parameter " + name);
}

bool AnimatorWindow::renameLayer(int index, const std::string& name){
    syncToDocument();
    ControllerDef def = working();
    if (index < 0 || (size_t)index >= def.layers.size() || name.empty()) return false;
    const std::string from = def.layers[(size_t)index].name;
    if (from == name) return false;
    if (def.findLayer(name) >= 0){
        status = "a layer \"" + name + "\" exists";
        return false;
    }
    def.layers[(size_t)index].name = name;
    if (!commit(def, "Rename Layer " + from + " -> " + name)) return false;
    if (index == layer) layerName = name;
    return true;
}

bool AnimatorWindow::moveLayer(int from, int to){
    syncToDocument();
    ControllerDef def = working();
    if (from < 0 || to < 0 || (size_t)from >= def.layers.size() || (size_t)to >= def.layers.size() || from == to) return false;
    LayerDef moved = def.layers[(size_t)from];
    def.layers.erase(def.layers.begin() + from);
    def.layers.insert(def.layers.begin() + to, moved);
    if (!commit(def, "Move Layer " + moved.name)) return false;
    // the current layer by name; the selection's indices are stale
    for (size_t l = 0; l < def.layers.size(); l++) if (def.layers[l].name == layerName) layer = (int)l;
    selection = Selection();
    return true;
}

bool AnimatorWindow::removeLayer(int index){
    syncToDocument();
    ControllerDef def = working();
    if (index < 0 || (size_t)index >= def.layers.size()) return false;
    if (def.layers.size() <= 1){
        status = "a controller keeps one layer";
        return false;
    }
    const std::string name = def.layers[(size_t)index].name;
    def.layers.erase(def.layers.begin() + index);
    if (!commit(def, "Delete Layer " + name)) return false;
    int found = -1;
    for (size_t l = 0; l < def.layers.size(); l++) if (def.layers[l].name == layerName) found = (int)l;
    if (found < 0){
        setCurrentLayer(0);
        canvas.frame({});
    }else{
        layer = found;
    }
    selection = Selection();
    return true;
}

bool AnimatorWindow::moveNodes(const std::vector<std::pair<std::string, ImVec2>>& positions){
    syncToDocument();
    if (!editable()) return false;
    ControllerDef def = working();
    StateMachineDef* m = machineOf(def);
    if (!m) return false;
    bool any = false;
    for (const auto& [name, position] : positions){
        const Vector2 p(position.x, position.y);
        if (name == "Entry"){ m->entryPosition = p; any = true; continue; }
        if (name == "Any State"){ m->anyStatePosition = p; any = true; continue; }
        if (name == "Exit"){ m->exitPosition = p; any = true; continue; }
        for (StateDef& st : m->states) if (st.name == name){ st.position = p; any = true; }
        for (StateMachineDef& sub : m->subMachines) if (sub.name == name){ sub.position = p; any = true; }
    }
    return any && commit(def, "Move States", "move");
}

int AnimatorWindow::setBlendThreshold(const std::string& state, int child, float threshold){
    syncToDocument();
    ControllerDef def = working();
    StateMachineDef* machine = machineOf(def);
    if (!machine || !std::isfinite(threshold)) return -1;
    for (StateDef& st : machine->states){
        if (st.name != state || !st.motion.isTree()) continue;
        BlendTreeDef& tree = st.motion.tree.front();
        if (tree.type != BlendTreeType::Simple1D || child < 0 || (size_t)child >= tree.children.size()) return -1;
        tree.children[(size_t)child].threshold = threshold;
        // threshold order (stable, as the file's normalization): the child keeps its motion
        std::vector<size_t> order(tree.children.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b){ return tree.children[a].threshold < tree.children[b].threshold; });
        std::vector<BlendChildDef> sorted;
        int landed = -1;
        for (size_t i = 0; i < order.size(); i++){
            sorted.push_back(tree.children[order[i]]);
            if ((int)order[i] == child) landed = (int)i;
        }
        tree.children = sorted;
        if (!commit(def, "Edit State " + state)) return -1;
        return landed;
    }
    return -1;
}

bool AnimatorWindow::resolveTypedAsset(const std::string& typed, bool clip, AnimatorAssetRef& out){
    out = AnimatorAssetRef();
    if (typed.empty()) return true;
    SubClipReference subClip;
    const bool isSubClip = clip && parseSubClipReference(typed, subClip);
    const std::string file = isSubClip ? subClip.model : typed;
    // absolute, project-relative ("assets/anim/run.anim") or assets-relative ("anim/run.anim")
    const fs::path given = fs::u8path(file);
    fs::path absolute = given.is_absolute() ? given : project->resolveAssetPath(given);
    std::error_code existsError;
    if (!given.is_absolute() && !fs::exists(absolute, existsError) && fs::exists(project->getProjectPath() / given, existsError)){
        absolute = project->getProjectPath() / given;
    }
    absolute = absolute.lexically_normal();
    if (!project->isInsideAssetsPath(absolute)){
        status = "\"" + typed + "\" is outside the assets folder";
        return false;
    }
    std::error_code ec;
    if (!fs::is_regular_file(absolute, ec)){
        status = "\"" + typed + "\" does not exist";
        return false;
    }
    const std::string stored = project->normalizeToAssetsRelative(absolute).generic_string();
    if (isSubClip){
        if (!Util::isModelFile(stored)){
            status = "\"" + subClip.model + "\" is not a model";
            return false;
        }
        out.path = formatSubClipReference(stored, subClip.clip);
        return true;
    }
    if (clip ? !Util::isAnimationClipFile(stored) : !Util::isAvatarMaskFile(stored)){
        status = "\"" + typed + "\" is not " + (clip ? "an Animation Clip (.anim)" : "an Avatar Mask (.mask)");
        return false;
    }
    out.path = stored;
    std::string text;
    out.guid = ProjectChecks::readFile(absolute, text) ? readAnimatorAssetGuid(text) : std::string();
    if (clip && out.guid.empty() && !text.empty()) out.guid = readClipGuid(text);
    return true;
}

std::vector<ClipWarning> AnimatorWindow::problems() const{
    if (!document) return {};
    Project* owner = project;
    return validateController(working(), [owner](const std::string& clip){
        ControllerClipInfo info;
        SubClipReference subClip;
        if (parseSubClipReference(clip, subClip)){
            std::error_code ec;
            info.loads = fs::is_regular_file(owner->resolveAssetPath(subClip.model), ec);
            if (!info.loads) info.error = "its model does not exist";
            return info;
        }
        std::string text;
        if (!ProjectChecks::readFile(owner->resolveAssetPath(clip), text)){
            info.error = "the file does not exist";
            return info;
        }
        ClipParseResult parsed = parseClip(text);
        info.loads = parsed.ok();
        if (!info.loads) info.error = parsed.error.toString();
        else info.loops = parsed.data->wrap == ClipWrapMode::Loop || parsed.data->wrap == ClipWrapMode::PingPong;
        return info;
    });
}

bool AnimatorWindow::selectPath(const std::string& path){
    syncToDocument();
    ControllerDef def = working();
    std::string rest = path;
    int index = -1;
    if (takeIndexed(rest, "parameters", index)){
        // the Parameters tab
        leftTab = 1;
        return index < (int)def.parameters.size();
    }
    int l = -1;
    if (!takeIndexed(rest, "layers", l) || l >= (int)def.layers.size()) return false;
    setCurrentLayer(l);
    selection = Selection();
    selection.kind = SelectionKind::Layer;
    canvas.frame({});
    if (rest.compare(0, 13, "state_machine") != 0) return true;
    rest = rest.substr(13);
    if (!rest.empty() && rest[0] == '.') rest = rest.substr(1);
    StateMachineDef* machine = &def.layers[(size_t)l].machine;
    while (true){
        int sub = -1;
        std::string probe = rest;
        if (takeIndexed(probe, "sub_state_machines", sub) && sub < (int)machine->subMachines.size()){
            // a finding inside a sub-machine: open it unless it is about the machine node itself
            if (probe.empty()){
                selection = Selection();
                selection.kind = SelectionKind::SubMachine;
                selection.name = machine->subMachines[(size_t)sub].name;
                return true;
            }
            if (probe.compare(0, 16, "exit_transitions") == 0){
                int t = -1;
                std::string after = probe;
                if (takeIndexed(after, "exit_transitions", t)){
                    selection = Selection();
                    selection.kind = SelectionKind::Transition;
                    selection.list = ListKind::Exit;
                    selection.owner = machine->subMachines[(size_t)sub].name;
                    selection.index = t;
                    return true;
                }
            }
            machinePath.push_back(machine->subMachines[(size_t)sub].name);
            machine = &machine->subMachines[(size_t)sub];
            rest = probe;
            continue;
        }
        break;
    }
    int i = -1;
    std::string probe = rest;
    if (takeIndexed(probe, "states", i) && i < (int)machine->states.size()){
        int t = -1;
        std::string after = probe;
        if (takeIndexed(after, "transitions", t)){
            selection = Selection();
            selection.kind = SelectionKind::Transition;
            selection.list = ListKind::State;
            selection.owner = machine->states[(size_t)i].name;
            selection.index = t;
            return true;
        }
        selectState(machine->states[(size_t)i].name);
        return true;
    }
    probe = rest;
    if (takeIndexed(probe, "any_state", i)){
        selection = Selection();
        selection.kind = SelectionKind::Transition;
        selection.list = ListKind::AnyState;
        selection.index = i;
        return true;
    }
    probe = rest;
    if (takeIndexed(probe, "entry", i)){
        selection = Selection();
        selection.kind = SelectionKind::Transition;
        selection.list = ListKind::Entry;
        selection.index = i;
        return true;
    }
    return true;
}

bool AnimatorWindow::inspect(const std::string& path){
    if (!selectPath(path)) return false;
    if (selection.kind != SelectionKind::None) claimInspector();
    return true;
}

bool AnimatorWindow::copySelection(){
    syncToDocument();
    if (selection.kind != SelectionKind::State) return false;
    ControllerDef def = working();
    StateMachineDef* machine = machineOf(def);
    if (!machine) return false;
    for (const StateDef& st : machine->states){
        if (st.name != selection.name) continue;
        clipboard = st;
        return true;
    }
    return false;
}

bool AnimatorWindow::paste(const ImVec2& position){
    syncToDocument();
    if (!clipboard || !editable()) return false;
    ControllerDef def = working();
    StateMachineDef* machine = machineOf(def);
    if (!machine) return false;
    StateDef state = *clipboard;
    const std::string original = state.name;
    state.name = uniqueName(*machine, original);
    state.position = Vector2(position.x, position.y);
    // only the transitions among the copied states (here: to itself) come along
    std::vector<TransitionDef> kept;
    for (TransitionDef t : state.transitions){
        if (t.to != original) continue;
        t.to = state.name;
        kept.push_back(t);
    }
    state.transitions = kept;
    machine->states.push_back(state);
    if (!commit(def, "Paste State " + state.name)) return false;
    selectState(state.name);
    return true;
}

bool AnimatorWindow::setLiveParameter(const std::string& name, float value){
    AnimatorInstance* live = liveInstance();
    if (!live) return false;
    const int index = live->findParameter(name);
    if (index < 0 || !document) return false;
    const int p = document->controller().findParameter(name);
    const AnimatorParameterType type = p >= 0 ? document->controller().parameters[(size_t)p].type : AnimatorParameterType::Float;
    if (type == AnimatorParameterType::Float) live->setFloat(name, value);
    else if (type == AnimatorParameterType::Int) live->setInteger(name, (int)std::lround(value));
    else if (type == AnimatorParameterType::Bool) live->setBool(name, value != 0.0f);
    else if (value != 0.0f) live->setTrigger(name);
    else live->resetTrigger(name);
    return true;
}

// ---------------------------------------------------------------- preview

bool AnimatorWindow::startPreview(){
    if (previewToken != 0) return true;
    SceneProject* scene = project->getScene(sceneId);
    if (!scene || !scene->scene || entity == NULL_ENTITY || scene->playState != ScenePlayState::STOPPED || !document){
        status = "Preview: no Animator in edit mode";
        return false;
    }
    if (!scene->scene->findComponent<AnimatorComponent>(entity)){
        status = "Preview: the GameObject has no Animator";
        return false;
    }
    const uint32_t id = sceneId;
    const Entity target = entity;
    previewToken = PreviewOwner::acquire(id, target, scene->scene, "Animator window", [this](){
        // stopped by another preview: the scene goes back (no release: the owner did)
        SceneProject* s = project->getScene(previewScene);
        if (s && s->scene){
            s->scene->getSystem<AnimatorSystem>()->restorePreview(previewEntity);
            particles.stop(s->scene);
        }
        previewToken = 0;
    });
    previewScene = id;
    previewEntity = target;
    previewClock = 0.0;
    previewPaused = false;
    lastFrameTime = nowSeconds();
    particles.stop(scene->scene);
    syncParticles(false);
    return true;
}

void AnimatorWindow::stopPreview(){
    if (previewToken == 0) return;
    const PreviewOwner::Token token = previewToken;
    previewToken = 0;
    SceneProject* scene = project->getScene(previewScene);
    if (scene && scene->scene){
        scene->scene->getSystem<AnimatorSystem>()->restorePreview(previewEntity);
        // the particle systems under it go back to stopped and empty
        particles.stop(scene->scene);
    }
    PreviewOwner::release(token);
}

void AnimatorWindow::syncParticles(bool forward){
    SceneProject* scene = project->getScene(previewScene);
    if (!scene || !scene->scene) return;
    particles.sync(scene->scene, previewEntity, previewClock, forward);
}

void AnimatorWindow::updatePreview(){
    if (previewToken == 0) return;
    if (!PreviewOwner::owns(previewToken)){
        previewToken = 0;
        return;
    }
    SceneProject* scene = project->getScene(previewScene);
    if (!scene || !scene->scene || scene->playState != ScenePlayState::STOPPED){
        stopPreview();
        return;
    }
    const double now = nowSeconds();
    const float dt = (float)std::min(0.1, std::max(0.0, now - lastFrameTime));
    lastFrameTime = now;
    particles.pump(scene->scene);
    if (previewPaused) return;
    stepPreview(dt);
}

bool AnimatorWindow::stepPreview(float dt){
    if (previewToken == 0 || !PreviewOwner::owns(previewToken)) return false;
    SceneProject* scene = project->getScene(previewScene);
    if (!scene || !scene->scene) return false;
    previewClock += dt;
    std::string error;
    if (!scene->scene->getSystem<AnimatorSystem>()->preview(previewEntity, dt, &error)){
        status = "Preview: " + error;
        return false;
    }
    syncParticles(true);
    scene->needUpdateRender = true;
    return true;
}

// ---------------------------------------------------------------- drawing

void AnimatorWindow::show(){
    if (!windowOpen) return;
    if (focusRequest){
        ImGui::SetNextWindowFocus();
        focusRequest = false;
    }
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)){
        windowFocused = false;
        ImGui::End();
        if (!windowOpen) stopPreview();
        return;
    }
    windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const Selection before = selection;
    struct ClaimOnChange{
        AnimatorWindow* window;
        Selection before;
        ~ClaimOnChange(){
            if (window->restoredByEsc) window->restoredByEsc = false;
            else if (window->selection.kind == SelectionKind::None) window->inspectorClaim = false;
            else if (!(window->selection == before)) window->claimInspector();
        }
    } claimOnChange{this, before};
    followSelection();
    updatePreview();
    drawToolbar();
    if (!document){
        ImGui::Spacing();
        ImGui::TextDisabled("%s", entity != NULL_ENTITY ? "The selected Animator has no controller: create one or pick a .controller."
                                                        : "Select a GameObject with an Animator, or open an Animator Controller (.controller).");
        ImGui::End();
        return;
    }
    syncToDocument();
    drawProblems();
    const float leftWidth = 230.0f * ImGui::GetStyle().FontScaleMain;
    ImGui::BeginChild("##animator_left", ImVec2(leftWidth, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    if (ImGui::BeginTabBar("##animator_tabs")){
        const bool layersTab = ImGui::BeginTabItem("Layers");
        UIProbe::item("animator.tab.layers");
        if (layersTab){
            leftTab = 0;
            drawLayers();
            ImGui::EndTabItem();
        }
        const bool parametersTab = ImGui::BeginTabItem("Parameters");
        UIProbe::item("animator.tab.parameters");
        if (parametersTab){
            leftTab = 1;
            drawParameters();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##animator_graph", ImVec2(0, 0), ImGuiChildFlags_None);
    drawGraph();
    ImGui::EndChild();
    ImGui::End();
    flush();
    if (!windowOpen) stopPreview();
}

void AnimatorWindow::drawToolbar(){
    const std::string name = document ? fs::path(document->projectPath()).filename().string() : std::string("(no controller)");
    ImGui::TextUnformatted(ICON_FA_DIAGRAM_PROJECT);
    ImGui::SameLine();
    ImGui::TextUnformatted(name.c_str());
    UIProbe::item("animator.path");
    if (document) ImGui::SetItemTooltip("%s", document->projectPath().c_str());
    ImGui::SameLine();
    if (ImGui::Button("Open...")){
        const std::string path = FileDialogs::openFileDialogFiltered(project->getAssetsPath().string(), "Animator Controllers", "controller");
        if (!path.empty()) openController(path, sceneId, entity);
    }
    UIProbe::item("animator.open");
    SceneProject* scene = project->getScene(sceneId);
    AnimatorComponent* animator = scene && scene->scene && entity != NULL_ENTITY && scene->scene->isEntityCreated(entity) ?
                                  scene->scene->findComponent<AnimatorComponent>(entity) : nullptr;
    if (animator){
        ImGui::SameLine();
        ImGui::TextDisabled("on %s", scene->scene->getEntityName(entity).c_str());
        ImGui::SameLine();
        ImGui::Checkbox("Pin", &pinned);
        ImGui::SetItemTooltip("Keep showing this Animator whatever is selected");
        if (scene->playState == ScenePlayState::STOPPED && document){
            ImGui::SameLine();
            bool previewing = previewToken != 0;
            if (ImGui::Checkbox("Preview", &previewing)){
                if (previewing) startPreview();
                else stopPreview();
            }
            UIProbe::item("animator.preview");
            ImGui::SetItemTooltip("Plays the controller on the Animator in edit mode (the scene is put back when it stops)");
            if (previewing){
                ImGui::SameLine();
                ImGui::Checkbox("Pause", &previewPaused);
            }
        }
    }
    if (!status.empty()){
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1.0f), "%s", status.c_str());
    }
    ImGui::Separator();
}

void AnimatorWindow::drawProblems(){
    if (!document) return;
    if (problemsRevision != document->revision() || problemsPath != document->projectPath()){
        problemsRevision = document->revision();
        problemsPath = document->projectPath();
        problemCache = problems();
    }
    if (problemCache.empty()) return;
    const std::string header = std::string(ICON_FA_TRIANGLE_EXCLAMATION " ") + std::to_string(problemCache.size()) + " problem(s)###animator_problems";
    if (ImGui::CollapsingHeader(header.c_str())){
        UIProbe::item("animator.problems");
        ImGui::BeginChild("##animator_problem_list", ImVec2(0, std::min(6.0f, (float)problemCache.size()) * ImGui::GetTextLineHeightWithSpacing() + 4.0f));
        for (size_t i = 0; i < problemCache.size(); i++){
            const ClipWarning& warning = problemCache[i];
            ImGui::PushID((int)i);
            const std::string line = warning.path.empty() ? warning.message : warning.path + ": " + warning.message;
            // a click selects the item (and shows it in the Inspector)
            if (ImGui::Selectable(line.c_str(), false)) inspect(warning.path);
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
}

void AnimatorWindow::drawLayers(){
    ControllerDef def = working();
    AnimatorInstance* live = liveInstance();
    if (ImGui::Button("+ Layer")) addLayer("New Layer");
    UIProbe::item("animator.addLayer");
    for (size_t l = 0; l < def.layers.size(); l++){
        ImGui::PushID((int)l);
        const bool selected = (int)l == layer;
        std::string label = def.layers[l].name;
        if (live && l < live->layers.size()) label += "  (" + std::to_string((int)std::lround(live->getLayerWeight((int)l) * 100)) + "%)";
        if (ImGui::Selectable(label.c_str(), selected)) setLayer((int)l);
        if (ImGui::BeginPopupContextItem("##layer_menu")){
            if (ImGui::MenuItem("Move Up", nullptr, false, l > 0)) moveLayer((int)l, (int)l - 1);
            if (ImGui::MenuItem("Move Down", nullptr, false, l + 1 < def.layers.size())) moveLayer((int)l, (int)l + 1);
            if (ImGui::MenuItem("Delete", nullptr, false, def.layers.size() > 1)) removeLayer((int)l);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
}

void AnimatorWindow::drawParameters(){
    ControllerDef def = working();
    AnimatorInstance* live = liveInstance();
    if (ImGui::Button("+")) ImGui::OpenPopup("##add_parameter");
    UIProbe::item("animator.addParameter");
    if (ImGui::BeginPopup("##add_parameter")){
        if (ImGui::MenuItem("Float")) addParameter("New Float", AnimatorParameterType::Float);
        if (ImGui::MenuItem("Int")) addParameter("New Int", AnimatorParameterType::Int);
        if (ImGui::MenuItem("Bool")) addParameter("New Bool", AnimatorParameterType::Bool);
        if (ImGui::MenuItem("Trigger")) addParameter("New Trigger", AnimatorParameterType::Trigger);
        ImGui::EndPopup();
    }
    for (size_t i = 0; i < def.parameters.size(); i++){
        const ParameterDef& p = def.parameters[i];
        ImGui::PushID((int)i);
        // the name: renamed on Enter (conditions and trees follow)
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%s", p.name.c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
        if (ImGui::InputText("##name", buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue)) renameParameter(p.name, buffer);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
        if (live){
            // the running value: edited live (not saved)
            const int index = live->findParameter(p.name);
            if (index >= 0){
                float value = live->parameters[(size_t)index];
                if (p.type == AnimatorParameterType::Float){
                    if (ImGui::DragFloat("##live", &value, 0.01f)) live->setFloat(p.name, value);
                }else if (p.type == AnimatorParameterType::Int){
                    int v = (int)std::lround(value);
                    if (ImGui::DragInt("##live", &v)) live->setInteger(p.name, v);
                }else if (p.type == AnimatorParameterType::Bool){
                    bool v = value != 0.0f;
                    if (ImGui::Checkbox("##live", &v)) live->setBool(p.name, v);
                }else{
                    if (ImGui::RadioButton("##live", value != 0.0f)) live->setTrigger(p.name);
                    ImGui::SetItemTooltip("Set the trigger");
                }
                // (UI tests: the running value's widget, not the default's)
                UIProbe::item("animator.parameter." + std::to_string(i) + ".live");
            }
        }else{
            ControllerDef full = def;
            ParameterDef& edited = full.parameters[i];
            bool changed = false;
            if (p.type == AnimatorParameterType::Float){
                changed = ImGui::DragFloat("##default", &edited.defaultValue, 0.01f);
            }else if (p.type == AnimatorParameterType::Int){
                int v = (int)std::lround(edited.defaultValue);
                if (ImGui::DragInt("##default", &v)){
                    edited.defaultValue = (float)v;
                    changed = true;
                }
            }else if (p.type == AnimatorParameterType::Bool){
                bool v = edited.defaultValue != 0.0f;
                if (ImGui::Checkbox("##default", &v)){
                    edited.defaultValue = v ? 1.0f : 0.0f;
                    changed = true;
                }
            }else{
                ImGui::TextDisabled("trigger");
            }
            if (changed) stage(full, "Set Parameter " + p.name);
        }
        UIProbe::item("animator.parameter." + std::to_string(i) + ".value");
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_XMARK)){
            removeParameter(def.parameters[i].name);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
}

void AnimatorWindow::drawGraph(){
    ControllerDef def = working();
    StateMachineDef* machine = machineOf(def);
    if (!machine){
        ImGui::TextDisabled("The controller has no layer: add one in the Layers tab.");
        return;
    }
    // breadcrumb
    {
        const std::string layerName = def.layers[(size_t)layer].name;
        if (ImGui::SmallButton(layerName.c_str())){
            machinePath.clear();
            selection = Selection();
            canvas.frame({});
        }
        for (size_t i = 0; i < machinePath.size(); i++){
            ImGui::SameLine();
            ImGui::TextDisabled(">");
            ImGui::SameLine();
            ImGui::PushID((int)i);
            if (ImGui::SmallButton(machinePath[i].c_str())){
                machinePath.resize(i + 1);
                selection = Selection();
                canvas.frame({});
            }
            ImGui::PopID();
        }
    }
    AnimatorInstance* live = liveInstance();
    const std::string prefix = def.layers[(size_t)layer].name + (machinePath.empty() ? std::string() : "." + [&](){
        std::string joined;
        for (const std::string& n : machinePath) joined += (joined.empty() ? "" : ".") + n;
        return joined;
    }()) + ".";
    std::string liveState, liveNext;
    float liveTime = -1.0f, liveProgress = -1.0f;
    if (live && (size_t)layer < live->layers.size()){
        const AnimatorInstance::StateInfo current = live->currentState(layer);
        liveState = current.fullPath;
        liveTime = current.normalizedTime - std::floor(current.normalizedTime);
        if (!current.loop) liveTime = std::min(1.0f, current.normalizedTime);
        if (live->isInTransition(layer)){
            liveNext = live->nextState(layer).fullPath;
            liveProgress = live->transitionProgress(layer);
        }
    }

    // nodes
    std::vector<GraphCanvas::Node> nodes;
    auto special = [&](uint64_t id, const char* label, const Vector2& position, ImU32 fill){
        GraphCanvas::Node node;
        node.id = id;
        node.label = label;
        node.position = ImVec2(position.x, position.y);
        node.size = ImVec2(120, 34);
        node.fill = fill;
        node.selected = isNodeSelected(label);
        nodes.push_back(node);
    };
    special(kEntry, "Entry", machine->entryPosition, kFillEntry);
    special(kAny, "Any State", machine->anyStatePosition, kFillAny);
    special(kExit, "Exit", machine->exitPosition, kFillExit);
    if (!machinePath.empty()) special(kUp, "(Up) Base", Vector2(machine->exitPosition.x, machine->exitPosition.y + 80), kFillUp);
    const std::string defaultName = machine->defaultState.empty() && !machine->states.empty() ? machine->states.front().name : machine->defaultState;
    for (size_t i = 0; i < machine->states.size(); i++){
        const StateDef& s = machine->states[i];
        GraphCanvas::Node node;
        node.id = kStateBase + i;
        node.label = s.name;
        node.subtitle = motionLabel(s.motion);
        node.position = ImVec2(s.position.x, s.position.y);
        node.size = ImVec2(180, 46);
        node.fill = s.name == defaultName ? kFillDefault : kFillState;
        node.selected = isNodeSelected(s.name);
        const std::string full = prefix + s.name;
        if (full == liveState){
            node.highlighted = true;
            node.progress = liveTime;
        }else if (full == liveNext){
            node.highlighted = true;
        }
        nodes.push_back(node);
    }
    for (size_t i = 0; i < machine->subMachines.size(); i++){
        const StateMachineDef& m = machine->subMachines[i];
        GraphCanvas::Node node;
        node.id = kMachineBase + i;
        node.label = m.name;
        node.subtitle = std::to_string(m.states.size()) + " state(s)";
        node.position = ImVec2(m.position.x, m.position.y);
        node.size = ImVec2(180, 46);
        node.fill = kFillMachine;
        node.rounded = false;
        node.selected = isNodeSelected(m.name);
        if (!liveState.empty() && liveState.rfind(prefix + m.name + ".", 0) == 0) node.highlighted = true;
        nodes.push_back(node);
    }

    // where `to` lands in this view
    auto target = [&](const std::string& to) -> uint64_t {
        if (to == "Exit") return kExit;
        std::string path = to;
        if (!path.empty() && path[0] == '/'){
            path = path.substr(1);
            std::string here;
            for (const std::string& n : machinePath) here += n + "/";
            if (path.rfind(here, 0) != 0) return machinePath.empty() ? 0 : kUp;
            path = path.substr(here.size());
        }
        const std::string first = path.substr(0, path.find('/'));
        for (size_t i = 0; i < machine->states.size(); i++) if (machine->states[i].name == first) return kStateBase + i;
        for (size_t i = 0; i < machine->subMachines.size(); i++) if (machine->subMachines[i].name == first) return kMachineBase + i;
        return 0;
    };
    std::vector<GraphCanvas::Link> links;
    auto addLinks = [&](uint64_t list, uint64_t owner, uint64_t from, const std::vector<TransitionDef>& transitions, const std::string& ownerName,
                        ListKind kind){
        for (size_t t = 0; t < transitions.size(); t++){
            const uint64_t to = target(transitions[t].to);
            if (to == 0) continue;
            GraphCanvas::Link link;
            link.id = linkId(list, owner, t);
            link.from = from;
            link.to = to;
            link.selected = selection.kind == SelectionKind::Transition && selection.list == kind && selection.owner == ownerName && selection.index == (int)t;
            if (transitions[t].mute) link.color = IM_COL32(140, 60, 60, 255);
            for (const GraphCanvas::Link& other : links){
                if (other.from == from && other.to == to) link.count++;
            }
            const std::string full = prefix + ownerName;
            if (!liveNext.empty() && full == liveState && to >= kStateBase && to < kMachineBase && prefix + machine->states[to - kStateBase].name == liveNext){
                link.progress = liveProgress;
            }
            links.push_back(link);
        }
    };
    for (size_t i = 0; i < machine->states.size(); i++){
        addLinks(kListState, i, kStateBase + i, machine->states[i].transitions, machine->states[i].name, ListKind::State);
    }
    addLinks(kListAny, 0, kAny, machine->anyState, "", ListKind::AnyState);
    addLinks(kListEntry, 0, kEntry, machine->entry, "", ListKind::Entry);
    for (size_t i = 0; i < machine->subMachines.size(); i++){
        addLinks(kListExit, i, kMachineBase + i, machine->subMachines[i].exitTransitions, machine->subMachines[i].name, ListKind::Exit);
    }
    // Entry's default: an orange arrow
    if (const uint64_t to = target(defaultName); to != 0){
        GraphCanvas::Link link;
        link.id = linkId(kListDefault, 0, 0);
        link.from = kEntry;
        link.to = to;
        link.color = kFillDefault;
        links.push_back(link);
    }

    GraphCanvas::Result result = canvas.draw("##animator_canvas", nodes, links);
    UIProbe::item("animator.canvas");
    // the nodes' screen rects (UI tests click them)
    for (const GraphCanvas::Node& node : nodes){
        const ImVec2 a = canvas.toScreen(node.position);
        const ImVec2 b = canvas.toScreen(ImVec2(node.position.x + node.size.x, node.position.y + node.size.y));
        if (node.id == kAny) UIProbe::rect("animator.any_state", a.x, a.y, b.x, b.y);
        else if (node.id == kEntry) UIProbe::rect("animator.entry", a.x, a.y, b.x, b.y);
        else if (node.id >= kStateBase && node.id < kMachineBase){
            UIProbe::rect("animator.state." + std::to_string(node.id - kStateBase), a.x, a.y, b.x, b.y);
            if (node.selected) UIProbe::rect("animator.state." + std::to_string(node.id - kStateBase) + ".selected", a.x, a.y, b.x, b.y);
        }
    }

    auto nameOf = [&](uint64_t id) -> std::string {
        if (id >= kMachineBase) return machine->subMachines[id - kMachineBase].name;
        if (id >= kStateBase) return machine->states[id - kStateBase].name;
        return std::string();
    };
    auto selectionName = [&](uint64_t id) -> std::string {
        if (id == kEntry) return "Entry";
        if (id == kAny) return "Any State";
        if (id == kExit) return "Exit";
        return nameOf(id);
    };
    auto isItem = [](uint64_t id){ return id >= kStateBase; };
    if (result.clickedNode || result.clickedBackground){
        selectionAtPress = selection;
        claimAtPress = inspectorClaim;
    }
    if (result.cancelled){
        selection = selectionAtPress;
        inspectorClaim = claimAtPress;
        restoredByEsc = true;
    }else if (result.selectionChanged && !result.clickedLink){
        // the canvas' nodes as the window's selection. The Inspector's node: the one clicked, else
        // the one it was while it stays selected, else the first state or sub-machine
        const auto& picked = result.selectedNodes;
        auto has = [&](uint64_t id){ return std::find(picked.begin(), picked.end(), id) != picked.end(); };
        uint64_t primary = isItem(result.clickedNode) && has(result.clickedNode) ? result.clickedNode : 0;
        for (uint64_t id : picked){
            if (primary == 0 && isItem(id) && selectionName(id) == selection.name) primary = id;
        }
        for (uint64_t id : picked){
            if (primary == 0 && isItem(id)) primary = id;
        }
        Selection next;
        if (primary != 0){
            next.kind = primary >= kMachineBase ? SelectionKind::SubMachine : SelectionKind::State;
            next.name = nameOf(primary);
        }
        for (uint64_t id : picked){
            if (id != primary && id != kUp) next.others.push_back(selectionName(id));
        }
        selection = next;
    }
    // a right click on a selected node: the menu and the Inspector act on it, the rest stay selected
    if (isItem(result.contextNode) && selection.name != nameOf(result.contextNode) && isNodeSelected(nameOf(result.contextNode))){
        std::replace(selection.others.begin(), selection.others.end(), nameOf(result.contextNode), selection.name);
        selection.kind = result.contextNode >= kMachineBase ? SelectionKind::SubMachine : SelectionKind::State;
        selection.name = nameOf(result.contextNode);
    }
    // a click, or the link a right click's menu deletes: never a node selection left behind
    if (const uint64_t pickedLink = result.clickedLink ? result.clickedLink : result.contextLink){
        const uint64_t list = pickedLink >> 40;
        const uint64_t owner = (pickedLink >> 20) & 0xFFFFF;
        const int index = (int)(pickedLink & 0xFFFFF);
        if (list != kListDefault){
            selection = Selection();
            selection.kind = SelectionKind::Transition;
            selection.index = index;
            if (list == kListState){ selection.list = ListKind::State; selection.owner = machine->states[owner].name; }
            else if (list == kListAny) selection.list = ListKind::AnyState;
            else if (list == kListEntry) selection.list = ListKind::Entry;
            else { selection.list = ListKind::Exit; selection.owner = machine->subMachines[owner].name; }
        }else if (result.contextLink){
            selection = Selection();
        }
    }
    if (result.doubleClickedNode >= kMachineBase){
        machinePath.push_back(nameOf(result.doubleClickedNode));
        selection = Selection();
        canvas.frame({});
    }else if (result.doubleClickedNode == kUp && !machinePath.empty()){
        machinePath.pop_back();
        selection = Selection();
        canvas.frame({});
    }
    if (!result.moved.empty() && editable()){
        std::vector<std::pair<std::string, ImVec2>> positions;
        for (const auto& [id, position] : result.moved){
            if (id == kEntry) positions.emplace_back("Entry", position);
            else if (id == kAny) positions.emplace_back("Any State", position);
            else if (id == kExit) positions.emplace_back("Exit", position);
            else if (id >= kMachineBase && id - kMachineBase < machine->subMachines.size()) positions.emplace_back(nameOf(id), position);
            else if (id >= kStateBase && id - kStateBase < machine->states.size()) positions.emplace_back(nameOf(id), position);
        }
        moveNodes(positions);
    }
    if (result.linkFrom && result.linkTo && editable()){
        std::string to;
        if (result.linkTo == kExit) to = "Exit";
        else if (result.linkTo >= kStateBase) to = nameOf(result.linkTo);
        if (!to.empty()){
            if (result.linkFrom == kAny) addTransition("", to, true, false);
            else if (result.linkFrom == kEntry) addTransition("", to, false, true);
            else addTransition(nameOf(result.linkFrom), to, false, false);
        }
    }
    if (result.contextRequested && editable()){
        // a node made from the menu is centred on the click (its box is 180 x 46)
        contextPosition = ImVec2(result.contextPosition.x - 90.0f, result.contextPosition.y - 23.0f);
        if (result.contextNode) ImGui::OpenPopup("##animator_node_menu");
        else if (result.contextLink) ImGui::OpenPopup("##animator_link_menu");
        else ImGui::OpenPopup("##animator_canvas_menu");
    }
    // (Delete is the command catalog's edit.delete: App's claim runs removeSelection while canDelete)

    // clips dropped from the Project window become states (a model offers its clips)
    // (the canvas is not "hovered" while a payload drags: the target is the graph child's rect)
    const ImRect dropRect(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                                                       ImGui::GetWindowPos().y + ImGui::GetWindowSize().y));
    if (ImGui::BeginDragDropTargetCustom(dropRect, ImGui::GetID("##animator_drop"))){
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
            std::vector<std::string> dropped = Util::getStringsFromPayload(payload);
            ImVec2 at = canvas.toCanvas(ImGui::GetMousePos());
            for (const std::string& file : dropped){
                if (Util::isAnimationClipFile(file)){
                    AnimatorAssetRef ref;
                    if (clipFromPath(project, file, ref)){
                        MotionDef motion;
                        motion.clip = ref;
                        addState(fs::path(file).stem().string(), at, motion);
                        at.y += 60.0f;
                    }
                }else if (Util::isModelFile(file)){
                    droppedModel = file;
                    dropPosition = at;
                    ImGui::OpenPopup("##animator_model_clips");
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    drawContextMenus();
}

void AnimatorWindow::drawContextMenus(){
    ControllerDef def = working();
    StateMachineDef* machine = machineOf(def);
    if (ImGui::BeginPopup("##animator_canvas_menu")){
        const bool createState = ImGui::BeginMenu("Create State");
        UIProbe::item("animator.menu.create_state");
        if (createState){
            const bool empty = ImGui::MenuItem("Empty");
            UIProbe::item("animator.menu.create_empty");
            if (empty) addState("New State", contextPosition, MotionDef());
            if (ImGui::MenuItem("From Clip...")){
                const std::string path = FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_ANIMATION_CLIP);
                AnimatorAssetRef ref;
                if (!path.empty() && clipFromPath(project, path, ref)){
                    MotionDef motion;
                    motion.clip = ref;
                    addState(fs::path(path).stem().string(), contextPosition, motion);
                }
            }
            if (ImGui::MenuItem("From New Blend Tree")){
                MotionDef motion;
                BlendTreeDef tree;
                for (const ParameterDef& p : def.parameters){
                    if (p.type == AnimatorParameterType::Float){
                        tree.parameter = p.name;
                        break;
                    }
                }
                motion.tree.push_back(tree);
                addState("Blend Tree", contextPosition, motion);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Create Sub-State Machine")) addSubStateMachine("New StateMachine", contextPosition);
        if (ImGui::MenuItem("Paste", nullptr, false, canPaste())) paste(contextPosition);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##animator_node_menu")){
        const bool isState = selection.kind == SelectionKind::State;
        const bool isMachine = selection.kind == SelectionKind::SubMachine;
        const bool makeTransition = ImGui::MenuItem("Make Transition", nullptr, false, isState || isMachine);
        UIProbe::item("animator.menu.make_transition");
        if (makeTransition){
            for (size_t i = 0; i < machine->states.size(); i++) if (machine->states[i].name == selection.name) canvas.beginLink(kStateBase + i);
            for (size_t i = 0; i < machine->subMachines.size(); i++) if (machine->subMachines[i].name == selection.name) canvas.beginLink(kMachineBase + i);
        }
        if (ImGui::MenuItem("Make Transition from Any State", nullptr, false, isState)) addTransition("", selection.name, true, false);
        if (ImGui::MenuItem("Set as Layer Default State", nullptr, false, isState || isMachine)) setDefaultState(selection.name);
        if (ImGui::MenuItem("Copy", nullptr, false, isState)) copySelection();
        if (isMachine && ImGui::MenuItem("Open")){
            machinePath.push_back(selection.name);
            selection = Selection();
            canvas.frame({});
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete", CommandCatalog::editor().menuShortcutText("edit.delete").c_str(), false, isState || isMachine)) removeSelection();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##animator_link_menu")){
        if (ImGui::MenuItem("Delete", CommandCatalog::editor().menuShortcutText("edit.delete").c_str())) removeSelection();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##animator_model_clips")){
        std::vector<ModelClipInfo> clips;
        std::string error;
        const std::string stored = project->normalizeToAssetsRelative(droppedModel).generic_string();
        if (listModelFileClips(stored, clips, &error)){
            ImGui::TextDisabled("Create a state from a clip of %s", fs::path(droppedModel).filename().string().c_str());
            for (const ModelClipInfo& clip : clips){
                if (ImGui::MenuItem(clip.name.c_str())){
                    MotionDef motion;
                    motion.clip.path = formatSubClipReference(stored, clip.name);
                    addState(clip.name, dropPosition, motion);
                }
            }
        }else{
            ImGui::TextDisabled("%s", error.c_str());
        }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------- Inspector panels

bool AnimatorWindow::drawSelectionInspector(){
    if (!windowOpen || !document || selection.kind == SelectionKind::None || !claimHolds()) return false;
    inspectorFrame = ImGui::GetFrameCount();
    ControllerDef def = working();
    StateMachineDef* machine = machineOf(def);
    if (!machine) return false;
    ImGui::PushID("animator_inspector");
    bool drew = false;
    if (selection.kind == SelectionKind::Layer){
        drawLayerInspector(def);
        drew = true;
    }else if (selection.kind == SelectionKind::State){
        for (StateDef& s : machine->states){
            if (s.name != selection.name) continue;
            drawStateInspector(def, s);
            drew = true;
            break;
        }
    }else if (selection.kind == SelectionKind::SubMachine){
        ImGui::Text(ICON_FA_DIAGRAM_PROJECT " Sub-State Machine  %s", selection.name.c_str());
        ImGui::TextDisabled("Double-click it in the Animator window to edit its states.");
        drew = true;
    }else if (selection.kind == SelectionKind::Transition){
        std::vector<TransitionDef>* list = nullptr;
        std::string from;
        if (selection.list == ListKind::AnyState){ list = &machine->anyState; from = "Any State"; }
        else if (selection.list == ListKind::Entry){ list = &machine->entry; from = "Entry"; }
        else if (selection.list == ListKind::Exit){
            for (StateMachineDef& sub : machine->subMachines) if (sub.name == selection.owner) list = &sub.exitTransitions;
            from = selection.owner + " (exit)";
        }else{
            for (StateDef& s : machine->states) if (s.name == selection.owner) list = &s.transitions;
            from = selection.owner;
        }
        if (list && selection.index >= 0 && (size_t)selection.index < list->size()){
            drawTransitionInspector(def, (*list)[(size_t)selection.index], from);
            drew = true;
        }
    }
    ImGui::PopID();
    flush();
    return drew;
}

void AnimatorWindow::drawLayerInspector(ControllerDef& def){
    if (layer < 0 || (size_t)layer >= def.layers.size()) return;
    LayerDef& l = def.layers[(size_t)layer];
    ImGui::Text(ICON_FA_LAYER_GROUP " Layer  %s", l.name.c_str());
    ImGui::Separator();
    bool changed = false;
    char name[128];
    std::snprintf(name, sizeof(name), "%s", l.name.c_str());
    if (ImGui::InputText("Name", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue) && name[0] && l.name != name){
        if (def.findLayer(name) >= 0){
            status = std::string("a layer \"") + name + "\" exists";
        }else{
            l.name = name;
            layerName = name;
            changed = true;
        }
    }
    ImGui::BeginDisabled(layer == 0);
    changed |= ImGui::SliderFloat("Weight", &l.weight, 0.0f, 1.0f);
    int blending = (int)l.blending;
    if (comboEnum("Blending", blending, {"Override", "Additive"})){
        l.blending = (AnimatorLayerBlending)blending;
        changed = true;
    }
    ImGui::EndDisabled();
    char mask[512];
    std::snprintf(mask, sizeof(mask), "%s", l.mask.path.c_str());
    if (ImGui::InputText("Mask", mask, sizeof(mask), ImGuiInputTextFlags_EnterReturnsTrue)){
        AnimatorAssetRef ref;
        if (resolveTypedAsset(mask, false, ref)){
            l.mask = ref;
            changed = true;
        }
    }
    if (ImGui::BeginDragDropTarget()){
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
            std::vector<std::string> dropped = Util::getStringsFromPayload(payload);
            if (!dropped.empty() && Util::isAvatarMaskFile(dropped[0]) && project->isInsideAssetsPath(dropped[0])){
                l.mask.path = project->normalizeToAssetsRelative(dropped[0]).generic_string();
                std::string text;
                l.mask.guid = ProjectChecks::readFile(dropped[0], text) ? readAnimatorAssetGuid(text) : std::string();
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SetItemTooltip("An Avatar Mask (.mask): drop one from the Project window");
    char slot[128];
    std::snprintf(slot, sizeof(slot), "%s", l.slot.c_str());
    if (ImGui::InputText("Montage Slot", slot, sizeof(slot), ImGuiInputTextFlags_EnterReturnsTrue)){
        l.slot = slot;
        changed = true;
    }
    ImGui::SetItemTooltip("Montages of this slot play on this layer (Default: the base layer without a slot)");
    if (changed) stage(def, "Edit Layer " + l.name);
}

void AnimatorWindow::drawStateInspector(ControllerDef& def, StateDef& state){
    ImGui::Text(ICON_FA_SQUARE " State  %s", state.name.c_str());
    ImGui::Separator();
    bool changed = false;
    std::string undo = "Edit State " + state.name;
    char name[128];
    std::snprintf(name, sizeof(name), "%s", state.name.c_str());
    // a rename is its own step (renameState: the transitions and the default follow), after
    // what this frame staged
    std::string renameTo;
    if (ImGui::InputText("Name", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue) && name[0] && state.name != name){
        renameTo = name;
    }
    // motion: a clip, or a blend tree
    int kind = state.motion.isTree() ? 1 : 0;
    if (comboEnum("Motion", kind, {"Clip", "Blend Tree"})){
        if (kind == 1 && !state.motion.isTree()){
            state.motion = MotionDef();
            state.motion.tree.emplace_back();
        }else if (kind == 0 && state.motion.isTree()){
            state.motion = MotionDef();
        }
        changed = true;
    }
    if (!state.motion.isTree()){
        char clip[512];
        std::snprintf(clip, sizeof(clip), "%s", state.motion.clip.path.c_str());
        if (ImGui::InputText("Clip", clip, sizeof(clip), ImGuiInputTextFlags_EnterReturnsTrue)){
            AnimatorAssetRef ref;
            if (resolveTypedAsset(clip, true, ref)){
                state.motion.clip = ref;
                changed = true;
            }
        }
        if (ImGui::BeginDragDropTarget()){
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
                std::vector<std::string> dropped = Util::getStringsFromPayload(payload);
                AnimatorAssetRef ref;
                if (!dropped.empty() && clipFromPath(project, dropped[0], ref)){
                    state.motion.clip = ref;
                    changed = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SetItemTooltip("An .anim, or a model clip \"models/hero.gltf#Walk\" (drop an .anim from the Project window)");
    }else{
        changed |= drawBlendTree(def, state.motion.tree.front(), 0);
    }
    changed |= ImGui::DragFloat("Speed", &state.speed, 0.01f);
    changed |= comboParameter("Multiplier", def, state.speedParameter, true);
    changed |= ImGui::DragFloat("Cycle Offset", &state.cycleOffset, 0.01f, 0.0f, 1.0f);
    changed |= comboParameter("Motion Time", def, state.timeParameter, true);
    int loop = (int)state.loop;
    if (comboEnum("Loop", loop, {"Inherit (the clip's)", "Loop", "Once", "Ping Pong", "Clamp Forever", "Repeat (forward)"})){
        state.loop = (AnimatorStateLoop)loop;
        changed = true;
    }
    char tag[128];
    std::snprintf(tag, sizeof(tag), "%s", state.tag.c_str());
    if (ImGui::InputText("Tag", tag, sizeof(tag), ImGuiInputTextFlags_EnterReturnsTrue)){
        state.tag = tag;
        changed = true;
    }
    if ((size_t)layer < def.layers.size() && def.layers[(size_t)layer].blending == AnimatorLayerBlending::Additive){
        int reference = (int)state.additiveReference;
        if (comboEnum("Additive Reference", reference, {"First Frame", "Rest Pose", "Time", "Clip's (additive_reference)"})){
            state.additiveReference = (AdditiveReference)reference;
            changed = true;
        }
        if (state.additiveReference == AdditiveReference::Time){
            changed |= ImGui::DragFloat("Reference Time", &state.additiveTime, 0.01f, 0.0f, 600.0f, "%.2f s");
        }
    }
    // humanoid playback (the one retarget path, S21 §12): Unity's state Mirror and Foot IK, and
    // upstream's Retarget (its clips retargeted whatever the Animator's Retarget)
    if (ImGui::Checkbox("Retarget", &state.retarget)) changed = true;
    UIProbe::item("animator.state.retarget");
    ImGui::SetItemTooltip("Play its clips retargeted from their source model, even with the Animator's Retarget Off");
    ImGui::SameLine();
    if (ImGui::Checkbox("Mirror", &state.mirror)) changed = true;
    UIProbe::item("animator.state.mirror");
    ImGui::SetItemTooltip("Humanoid playback: left and right swapped (needs a humanoid avatar)");
    ImGui::SameLine();
    if (ImGui::Checkbox("Foot IK", &state.footIK)) changed = true;
    UIProbe::item("animator.state.footIK");
    ImGui::SetItemTooltip("Humanoid playback: the feet reach the clip's foot goals (needs a humanoid avatar)");
    ImGui::SeparatorText("Transitions");
    for (size_t t = 0; t < state.transitions.size(); t++){
        ImGui::PushID((int)t);
        const std::string label = state.name + " -> " + state.transitions[t].to + (state.transitions[t].mute ? "  (muted)" : "");
        if (ImGui::Selectable(label.c_str(), false)){
            Selection next;
            next.kind = SelectionKind::Transition;
            next.list = ListKind::State;
            next.owner = state.name;
            next.index = (int)t;
            selection = next;
        }
        ImGui::PopID();
    }
    if (changed) stage(def, undo);
    if (!renameTo.empty()){
        const std::string from = state.name;
        if (pending){
            const ControllerDef staged = *pending;
            const std::string stagedUndo = pendingUndo;
            commit(staged, stagedUndo);
        }
        renameState(from, renameTo);
    }
}

bool AnimatorWindow::drawBlendTree(ControllerDef& def, BlendTreeDef& tree, int depth){
    bool changed = false;
    ImGui::PushID(depth);
    int type = (int)tree.type;
    if (comboEnum("Blend Type", type, {"1D", "2D Simple Directional", "2D Freeform Directional", "2D Freeform Cartesian", "Direct"})){
        tree.type = (BlendTreeType)type;
        changed = true;
    }
    const bool twoD = tree.type == BlendTreeType::SimpleDirectional2D || tree.type == BlendTreeType::FreeformDirectional2D ||
                      tree.type == BlendTreeType::FreeformCartesian2D;
    if (tree.type != BlendTreeType::Direct) changed |= comboParameter(twoD ? "Parameter X" : "Parameter", def, tree.parameter, true);
    if (twoD) changed |= comboParameter("Parameter Y", def, tree.parameterY, true);

    // the weights at the current (live, else default) parameters, from the compiled tree
    ControllerDef single;
    single.parameters = def.parameters;
    LayerDef l;
    StateDef s;
    s.name = "Tree";
    s.motion.tree.push_back(tree);
    l.machine.states.push_back(s);
    single.layers.push_back(l);
    normalizeController(single);
    std::shared_ptr<const CompiledController> compiled = compileController(single);
    std::vector<float> parameters(def.parameters.size());
    AnimatorInstance* live = liveInstance();
    for (size_t p = 0; p < def.parameters.size(); p++){
        parameters[p] = def.parameters[p].defaultValue;
        if (live){
            const int index = live->findParameter(def.parameters[p].name);
            if (index >= 0) parameters[p] = live->parameters[(size_t)index];
        }
    }
    std::vector<float> weights(compiled->trees.empty() ? 0 : compiled->trees[0].childCount, 0.0f);
    if (!compiled->trees.empty() && !parameters.empty()) blendTreeWeights(*compiled, compiled->trees[0], parameters.data(), weights.data());

    if (ImGui::BeginTable("##children", tree.type == BlendTreeType::Simple1D || tree.type == BlendTreeType::Direct ? 5 : 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)){
        ImGui::TableSetupColumn("Motion");
        if (tree.type == BlendTreeType::Simple1D) ImGui::TableSetupColumn("Threshold");
        else if (tree.type == BlendTreeType::Direct) ImGui::TableSetupColumn("Parameter");
        else { ImGui::TableSetupColumn("Pos X"); ImGui::TableSetupColumn("Pos Y"); }
        ImGui::TableSetupColumn("Speed");
        ImGui::TableSetupColumn("Mirror");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (size_t c = 0; c < tree.children.size(); c++){
            BlendChildDef& child = tree.children[c];
            ImGui::PushID((int)c);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string label = child.motion.isTree() ? std::string("(Blend Tree)") : (child.motion.clip.empty() ? std::string("(drop a clip)") : motionLabel(child.motion));
            const float w = c < weights.size() ? weights[c] : 0.0f;
            ImGui::Text("%s  %.0f%%", label.c_str(), w * 100.0f);
            if (ImGui::BeginDragDropTarget()){
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
                    std::vector<std::string> dropped = Util::getStringsFromPayload(payload);
                    AnimatorAssetRef ref;
                    if (!dropped.empty() && clipFromPath(project, dropped[0], ref)){
                        child.motion = MotionDef();
                        child.motion.clip = ref;
                        changed = true;
                    }
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (tree.type == BlendTreeType::Simple1D){
                changed |= ImGui::DragFloat("##threshold", &child.threshold, 0.01f);
            }else if (tree.type == BlendTreeType::Direct){
                changed |= comboParameter("##direct", def, child.directParameter, true);
            }else{
                changed |= ImGui::DragFloat("##x", &child.position.x, 0.01f);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                changed |= ImGui::DragFloat("##y", &child.position.y, 0.01f);
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            changed |= ImGui::DragFloat("##speed", &child.timeScale, 0.01f, 0.01f, 10.0f);
            ImGui::TableNextColumn();
            changed |= ImGui::Checkbox("##mirror", &child.mirror);
            ImGui::SetItemTooltip("Flips the state's Mirror for this motion (humanoid playback)");
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(ICON_FA_XMARK)){
                tree.children.erase(tree.children.begin() + (long)c);
                changed = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (tree.children.size() < MAX_BLEND_CHILDREN && ImGui::Button("+ Motion")){
        BlendChildDef child;
        if (!tree.children.empty()){
            child.threshold = tree.children.back().threshold + 1.0f;
            child.position = Vector2(tree.children.back().position.x + 1.0f, tree.children.back().position.y);
        }
        tree.children.push_back(child);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(twoD ? "Compute Positions" : "Compute Thresholds")){
        // from each clip's root motion: its speed (1D) or its velocity on the ground (2D)
        for (BlendChildDef& child : tree.children){
            if (child.motion.isTree() || child.motion.clip.empty()) continue;
            std::string error;
            std::shared_ptr<const AnimationClip> clip = AnimationClipPool::getOrLoad(child.motion.clip.path, &error);
            if (!clip || !(clip->getLength() > 0.0f)) continue;
            // the shallowest position track: what root motion would move
            const AnimationClip::Track* best = nullptr;
            size_t bestDepth = 1u << 30;
            for (const AnimationClip::Track& track : clip->getTracks()){
                if (track.channel != ClipChannel::Position || !track.isSampled()) continue;
                const size_t d = (size_t)std::count(track.path.begin(), track.path.end(), '/');
                if (d < bestDepth){
                    bestDepth = d;
                    best = &track;
                }
            }
            if (!best) continue;
            TrackSample a, b;
            sampleTrack(*best, 0.0f, a);
            sampleTrack(*best, clip->getLength(), b);
            const Vector3 velocity = (b.vector - a.vector) * (1.0f / clip->getLength());
            if (twoD) child.position = Vector2(velocity.x, velocity.z);
            else child.threshold = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
        }
        changed = true;
    }
    ImGui::SetItemTooltip("From each clip's root motion: its speed (1D) or its velocity on the ground (2D)");

    // the weights: 1D curves, 2D samples with the parameter point and weight rings
    if (!compiled->trees.empty() && tree.type != BlendTreeType::Direct && !tree.children.empty()){
        const ImVec2 size(ImGui::GetContentRegionAvail().x, 140.0f);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##tree_view", size);
        ImDrawList* list = ImGui::GetWindowDrawList();
        list->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(32, 32, 36, 255));
        const int px = compiled->trees[0].parameterX;
        const int py = compiled->trees[0].parameterY;
        if (tree.type == BlendTreeType::Simple1D){
            float lo = tree.children.front().threshold, hi = tree.children.back().threshold;
            for (const BlendChildDef& c : tree.children){ lo = std::min(lo, c.threshold); hi = std::max(hi, c.threshold); }
            if (hi - lo < 1e-4f) hi = lo + 1.0f;
            std::vector<float> curve(tree.children.size());
            for (size_t c = 0; c < tree.children.size(); c++){
                ImVec2 previous;
                for (int x = 0; x <= 64; x++){
                    std::vector<float> p = parameters;
                    if (px >= 0) p[(size_t)px] = lo + (hi - lo) * (float)x / 64.0f;
                    blendTreeWeights(*compiled, compiled->trees[0], p.data(), curve.data());
                    const ImVec2 point(origin.x + size.x * (float)x / 64.0f, origin.y + size.y - 6.0f - (size.y - 12.0f) * curve[c]);
                    if (x > 0) list->AddLine(previous, point, ImGui::GetColorU32(ImGuiCol_PlotLines, 0.4f + 0.6f * (float)c / std::max<size_t>(1, tree.children.size())), 1.5f);
                    previous = point;
                }
            }
            if (px >= 0){
                const float x = origin.x + size.x * (parameters[(size_t)px] - lo) / (hi - lo);
                list->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + size.y), IM_COL32(255, 80, 80, 255), 2.0f);
                if (ImGui::IsItemActive() && live){
                    const float value = lo + (hi - lo) * std::min(1.0f, std::max(0.0f, (ImGui::GetIO().MousePos.x - origin.x) / size.x));
                    live->setFloat(def.parameters[(size_t)px].name, value);
                }
            }
        }else{
            float extent = 1.0f;
            for (const BlendChildDef& c : tree.children) extent = std::max(extent, std::max(std::fabs(c.position.x), std::fabs(c.position.y)));
            extent *= 1.15f;
            const ImVec2 centre(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);
            const float scale = std::min(size.x, size.y) * 0.5f / extent;
            for (size_t c = 0; c < tree.children.size(); c++){
                const ImVec2 point(centre.x + tree.children[c].position.x * scale, centre.y - tree.children[c].position.y * scale);
                list->AddCircleFilled(point, 4.0f, IM_COL32(90, 160, 255, 255));
                if (c < weights.size() && weights[c] > 0.0f) list->AddCircle(point, 4.0f + 14.0f * weights[c], IM_COL32(90, 160, 255, 200), 0, 2.0f);
            }
            if (px >= 0 && py >= 0){
                const ImVec2 point(centre.x + parameters[(size_t)px] * scale, centre.y - parameters[(size_t)py] * scale);
                list->AddCircleFilled(point, 5.0f, IM_COL32(255, 80, 80, 255));
                if (ImGui::IsItemActive() && live){
                    const ImVec2 m = ImGui::GetIO().MousePos;
                    live->setFloat(def.parameters[(size_t)px].name, (m.x - centre.x) / scale);
                    live->setFloat(def.parameters[(size_t)py].name, -(m.y - centre.y) / scale);
                }
            }
        }
        ImGui::SetItemTooltip("Drag the red marker while playing or previewing to set the parameters");
    }
    ImGui::PopID();
    return changed;
}

void AnimatorWindow::drawTransitionInspector(ControllerDef& def, TransitionDef& t, const std::string& from){
    ImGui::Text(ICON_FA_ARROW_RIGHT " Transition  %s -> %s", from.c_str(), t.to.c_str());
    UIProbe::item("animator.transition.title");
    ImGui::Separator();
    bool changed = false;
    const bool entry = selection.list == ListKind::Entry || selection.list == ListKind::Exit;
    changed |= ImGui::Checkbox("Mute", &t.mute);
    if (selection.list == ListKind::AnyState) changed |= ImGui::Checkbox("Can Transition To Self", &t.canTransitionToSelf);
    if (!entry){
        changed |= ImGui::Checkbox("Has Exit Time", &t.hasExitTime);
        ImGui::BeginDisabled(!t.hasExitTime);
        changed |= ImGui::DragFloat("Exit Time", &t.exitTime, 0.01f, 0.0f, 100.0f);
        ImGui::EndDisabled();
        changed |= ImGui::Checkbox("Fixed Duration", &t.fixedDuration);
        changed |= ImGui::DragFloat(t.fixedDuration ? "Duration (s)" : "Duration (%)", &t.duration, 0.01f, 0.0f, 100.0f);
        changed |= ImGui::DragFloat("Offset", &t.offset, 0.01f, 0.0f, 1.0f);
        int source = (int)t.interruptionSource;
        if (comboEnum("Interruption Source", source, {"None", "Current State", "Next State", "Current State Then Next State", "Next State Then Current State"})){
            t.interruptionSource = (TransitionInterruptionSource)source;
            changed = true;
        }
        changed |= ImGui::Checkbox("Ordered Interruption", &t.orderedInterruption);
    }
    ImGui::SeparatorText("Conditions");
    for (size_t c = 0; c < t.conditions.size(); c++){
        ConditionDef& condition = t.conditions[c];
        ImGui::PushID((int)c);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
        changed |= comboParameter("##parameter", def, condition.parameter, false);
        const int p = def.findParameter(condition.parameter);
        const AnimatorParameterType type = p >= 0 ? def.parameters[(size_t)p].type : AnimatorParameterType::Float;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
        if (type == AnimatorParameterType::Float){
            int mode = condition.mode == AnimatorConditionMode::Less ? 1 : 0;
            if (comboEnum("##mode", mode, {"Greater", "Less"})){ condition.mode = mode ? AnimatorConditionMode::Less : AnimatorConditionMode::Greater; changed = true; }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
            changed |= ImGui::DragFloat("##threshold", &condition.threshold, 0.01f);
        }else if (type == AnimatorParameterType::Int){
            int mode = condition.mode == AnimatorConditionMode::Less ? 1 : (condition.mode == AnimatorConditionMode::Equals ? 2 :
                       (condition.mode == AnimatorConditionMode::NotEqual ? 3 : 0));
            if (comboEnum("##mode", mode, {"Greater", "Less", "Equals", "NotEqual"})){
                const AnimatorConditionMode modes[] = {AnimatorConditionMode::Greater, AnimatorConditionMode::Less, AnimatorConditionMode::Equals, AnimatorConditionMode::NotEqual};
                condition.mode = modes[mode];
                changed = true;
            }
            ImGui::SameLine();
            int v = (int)std::lround(condition.threshold);
            ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
            if (ImGui::DragInt("##threshold", &v)){
                condition.threshold = (float)v;
                changed = true;
            }
        }else if (type == AnimatorParameterType::Bool){
            int mode = condition.mode == AnimatorConditionMode::IfNot ? 1 : 0;
            if (comboEnum("##mode", mode, {"true", "false"})){ condition.mode = mode ? AnimatorConditionMode::IfNot : AnimatorConditionMode::If; changed = true; }
        }else{
            condition.mode = AnimatorConditionMode::If;
            ImGui::TextDisabled("(trigger)");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_XMARK)){
            t.conditions.erase(t.conditions.begin() + (long)c);
            changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    const bool addCondition = ImGui::Button("+ Condition");
    UIProbe::item("animator.transition.add_condition");
    if (addCondition && !def.parameters.empty()){
        ConditionDef condition;
        condition.parameter = def.parameters.front().name;
        condition.mode = def.parameters.front().type == AnimatorParameterType::Float ? AnimatorConditionMode::Greater :
                         (def.parameters.front().type == AnimatorParameterType::Int ? AnimatorConditionMode::Equals : AnimatorConditionMode::If);
        t.conditions.push_back(condition);
        changed = true;
    }
    if (def.parameters.empty()) ImGui::TextDisabled("Add parameters in the Animator window to make conditions.");

    // a scrub of the blend on the previewed Animator (between two states of this machine)
    StateMachineDef* machine = machineOf(def);
    bool toState = false;
    for (const StateDef& s : machine->states) if (s.name == t.to) toState = true;
    if (previewToken != 0 && selection.list == ListKind::State && !selection.owner.empty() && toState){
        ImGui::SeparatorText("Preview");
        if (ImGui::SliderFloat("Blend", &transitionScrub, 0.0f, 1.0f)){
            previewPaused = true;
            SceneProject* scene = project->getScene(previewScene);
            AnimatorComponent* animator = scene && scene->scene ? scene->scene->findComponent<AnimatorComponent>(previewEntity) : nullptr;
            if (animator && animator->runtime){
                AnimatorSystem* system = scene->scene->getSystem<AnimatorSystem>().get();
                std::string prefix = def.layers[(size_t)layer].name + ".";
                for (const std::string& name : machinePath) prefix += name + ".";
                std::string error;
                // from the source's start: enter it, then the transition, then advance into it
                if (animator->runtime->play(prefix + selection.owner, layer, 0.0f) && system->preview(previewEntity, 0.0f, &error)){
                    const float seconds = t.fixedDuration ? t.duration : t.duration * animator->runtime->currentState(layer).length;
                    if (animator->runtime->crossFade(prefix + t.to, t.duration, t.fixedDuration, layer, t.offset)){
                        system->preview(previewEntity, transitionScrub * seconds, &error);
                    }
                    // the effects under it at the scrubbed point of the transition
                    previewClock = transitionScrub * seconds;
                    syncParticles(false);
                }
                if (!error.empty()) status = "Preview: " + error;
                scene->needUpdateRender = true;
            }
        }
        ImGui::SetItemTooltip("Shows the transition's blend at that point on the previewed GameObject (the preview pauses)");
    }
    if (changed) stage(def, "Edit Transition " + from + " -> " + t.to);
}
