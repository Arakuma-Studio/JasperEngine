// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PoseEditSession.h"

#include "Project.h"
#include "Scene.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "pose/PoseAnimationDocument.h"
#include "pose/PoseIK.h"
#include "pose/PoseMath.h"
#include "pose/PoseRigProvider.h"
#include "pose/PoseUiLogic.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace {

    editor::PoseEditSession* activeSession = nullptr;

    bool sameBody(const pose::BodyPose& a, const pose::BodyPose& b){
        return a.angles == b.angles && a.offset == b.offset;
    }

}

editor::PoseEditSession* editor::PoseEditSession::active(){
    return activeSession;
}

void editor::PoseEditSession::setActive(PoseEditSession* session){
    activeSession = session;
}

editor::PoseEditSession::PoseEditSession(Project* project): projectPtr(project){
}

editor::PoseEditSession::~PoseEditSession(){
    if (activeSession == this){
        activeSession = nullptr;
    }
    unbind();
    closeDocument();
}

// ---- listeners ----

void editor::PoseEditSession::addListener(std::function<void()> listener){
    if (listener){
        listeners.push_back(std::move(listener));
    }
}

void editor::PoseEditSession::notify(){
    if (notifying) return;   // a listener that changes the session does not recurse
    notifying = true;
    const std::vector<std::function<void()>> copy = listeners;
    for (const auto& listener : copy){
        listener();
    }
    notifying = false;
}

// ---- binding ----

bool editor::PoseEditSession::bind(Scene* scene, uint32_t sceneId, Entity model, std::string* error){
    auto fail = [error](const std::string& message){
        if (error) *error = message;
        return false;
    };
    if (!projectPtr || !scene || model == NULL_ENTITY || !scene->isEntityCreated(model)){
        return fail("no model GameObject");
    }
    ModelComponent* modelComponent = scene->findComponent<ModelComponent>(model);
    if (!modelComponent){
        return fail("'" + scene->getEntityName(model) + "' has no Model component");
    }
    if (modelComponent->skeleton == NULL_ENTITY || modelComponent->bonesIdMapping.empty()){
        return fail("'" + scene->getEntityName(model) + "' has no skeleton (bones): pose animations need a skinned humanoid model");
    }
    std::shared_ptr<const PoseRigInfo> info = PoseRigProvider::forSceneModel(projectPtr, scene, model);
    if (!info || !info->rig || !info->rig->valid()){
        return fail(info && !info->error.empty() ? info->error : "this model's bones could not be mapped to a humanoid");
    }
    if (bound() && scene == scenePtr && model == modelEntity){
        // the same model again (a new mapping): keep the pose shown, on the new rig
        restoreBones();
    }else{
        unbind();
    }
    scenePtr = scene;
    sceneIdValue = sceneId;
    modelEntity = model;
    modelPath = info->model;
    rigInfoPtr = info;
    rigPtr = info->rig;
    workingPose = rigPtr->restPose();
    shown = workingPose;
    if (currentMode != Mode::Idle && doc && poseCount() > 0){
        applyCurrent();
    }else{
        currentMode = Mode::Idle;
    }
    notify();
    return true;
}

void editor::PoseEditSession::unbind(){
    if (globalEdit){
        globalEdit = false;
        globalEditPose = -1;
    }
    restoreBones();
    dropBinding();
}

void editor::PoseEditSession::dropBinding(){
    const bool wasBound = bound();
    scenePtr = nullptr;
    sceneIdValue = 0;
    modelEntity = NULL_ENTITY;
    modelPath.clear();
    rigPtr.reset();
    rigInfoPtr.reset();
    savedBones.clear();
    applied = false;
    suspended = false;
    playing = false;
    currentMode = Mode::Idle;
    globalEdit = false;
    globalEditPose = -1;
    if (wasBound){
        notify();
    }
}

void editor::PoseEditSession::clearSceneState(uint32_t sceneId){
    if (bound() && sceneIdValue == sceneId){
        // the scene and its entities are gone: nothing to restore
        dropBinding();
    }
}

Entity editor::PoseEditSession::model() const { return modelEntity; }
Scene* editor::PoseEditSession::scene() const { return scenePtr; }
uint32_t editor::PoseEditSession::sceneId() const { return sceneIdValue; }
const std::shared_ptr<pose::PoseRig>& editor::PoseEditSession::rig() const { return rigPtr; }

// ---- document ----

bool editor::PoseEditSession::open(const std::string& poseanimPath, std::string* error){
    std::shared_ptr<PoseAnimationDocument> opened = PoseAnimationDocument::open(projectPtr, poseanimPath, error);
    if (!opened){
        return false;
    }
    if (opened == doc){
        return true;
    }
    closeDocument();
    doc = opened;
    documentListener = doc->addListener([this](PoseAnimationDocument&, PoseAnimationDocument::Change change){
        documentChanged(static_cast<int>(change));
    });
    selected.clear();
    primaryIndex = -1;
    anchorIndex = -1;
    time = 0.0f;
    clampSelectionToDocument();
    notify();
    return true;
}

void editor::PoseEditSession::closeDocument(){
    if (!doc) return;
    cancelGlobalEdit();
    goIdle();
    if (documentListener){
        doc->removeListener(documentListener);
        documentListener = 0;
    }
    doc.reset();
    selected.clear();
    primaryIndex = -1;
    anchorIndex = -1;
    time = 0.0f;
    playing = false;
    notify();
}

const std::shared_ptr<editor::PoseAnimationDocument>& editor::PoseEditSession::document() const { return doc; }

int editor::PoseEditSession::poseCount() const{
    return doc ? static_cast<int>(doc->data().poses.size()) : 0;
}

void editor::PoseEditSession::clampSelectionToDocument(){
    poseui::Selection sel{primaryIndex, anchorIndex, selected};
    poseui::clampSelection(sel, poseCount());
    primaryIndex = sel.primary;
    anchorIndex = sel.anchor;
    selected = sel.indices;
}

void editor::PoseEditSession::documentChanged(int change){
    const auto kind = static_cast<PoseAnimationDocument::Change>(change);
    if (kind == PoseAnimationDocument::Change::Removed){
        // an undone creation: nothing to show until the file is back
        globalEdit = false;
        globalEditPose = -1;
        goIdle();
        clampSelectionToDocument();
        notify();
        return;
    }
    clampSelectionToDocument();
    if (globalEdit && (globalEditPose < 0 || globalEditPose >= poseCount())){
        globalEdit = false;
        globalEditPose = -1;
    }
    if (poseCount() == 0 && currentMode != Mode::Idle){
        goIdle();
    }else if (currentMode != Mode::Idle && !suspended){
        // undo/redo (and every commit) shows the primary pose as the document has it now
        if (currentMode == Mode::Posing){
            workingPose = keyPose(primaryIndex);
        }
        applyCurrent();
    }
    notify();
}

std::shared_ptr<const editor::PoseRealisation> editor::PoseEditSession::realisation(){
    if (!doc || !rigPtr || !doc->exists()){
        return nullptr;
    }
    return doc->realise(rigPtr);
}

pose::HumanPose editor::PoseEditSession::keyPose(int index){
    std::shared_ptr<const PoseRealisation> realised = realisation();
    if (realised && index >= 0 && index < static_cast<int>(realised->keys.size())){
        return realised->keys[index];
    }
    return rigPtr ? rigPtr->restPose() : pose::HumanPose();
}

bool editor::PoseEditSession::commitData(const pose::PoseAnimationData& data, const std::string& undoName,
                                         const std::string& mergeKey, const std::vector<int>* selection, int primary,
                                         std::string* error, bool keepGlobalEdit){
    if (!doc){
        if (error) *error = "no pose animation is open";
        return false;
    }
    if (!keepGlobalEdit){
        cancelGlobalEdit();
    }
    // the selection first: the commit's document notification shows its primary pose
    if (selection){
        selected = *selection;
        if (primary >= 0) primaryIndex = primary;
        anchorIndex = primaryIndex;
    }else if (primary >= 0){
        primaryIndex = primary;
    }
    std::vector<pose::ClampReport> clamped;
    std::string failure;
    if (!doc->commit(data, undoName, mergeKey, &clamped, &failure)){
        if (error) *error = failure;
        clampSelectionToDocument();
        notify();
        return false;
    }
    clampSelectionToDocument();
    if (currentMode == Mode::Posing && !suspended){
        workingPose = keyPose(primaryIndex);
        applyCurrent();
    }
    notify();
    return true;
}

// ---- selection ----

int editor::PoseEditSession::primary() const { return primaryIndex; }
const std::vector<int>& editor::PoseEditSession::selection() const { return selected; }

void editor::PoseEditSession::select(int index, bool additive, bool range){
    if (index < 0 || index >= poseCount()) return;
    poseui::Selection sel{primaryIndex, anchorIndex, selected};
    poseui::selectPose(sel, index, additive, range, poseCount());
    if (sel.primary != primaryIndex){
        // selecting another pose cancels Global Edit (the edited pose goes back to its baseline)
        cancelGlobalEdit();
    }
    const bool primaryChanged = sel.primary != primaryIndex;
    primaryIndex = sel.primary;
    anchorIndex = sel.anchor;
    selected = sel.indices;
    if (primaryChanged && currentMode == Mode::Posing){
        workingPose = keyPose(primaryIndex);
        applyCurrent();
    }
    notify();
}

void editor::PoseEditSession::setSelection(const std::vector<int>& indices, int primary){
    if (primary != primaryIndex) cancelGlobalEdit();
    const bool primaryChanged = primary != primaryIndex;
    selected = indices;
    primaryIndex = primary;
    anchorIndex = primary;
    clampSelectionToDocument();
    if (primaryChanged && currentMode == Mode::Posing){
        workingPose = keyPose(primaryIndex);
        applyCurrent();
    }
    notify();
}

// ---- modes ----

editor::PoseEditSession::Mode editor::PoseEditSession::mode() const { return currentMode; }

void editor::PoseEditSession::startPosing(){
    if (!bound() || !doc || poseCount() == 0 || !bonesValid()) return;
    clampSelectionToDocument();
    playing = false;
    currentMode = Mode::Posing;
    suspended = false;
    workingPose = keyPose(primaryIndex);
    applyCurrent();
    notify();
}

void editor::PoseEditSession::startPreview(){
    if (!bound() || !doc || poseCount() == 0 || !bonesValid()) return;
    cancelGlobalEdit();
    currentMode = Mode::Previewing;
    suspended = false;
    endPause = 0.0f;
    const float length = previewLength();
    if (time > length) time = 0.0f;
    applyCurrent();
    notify();
}

void editor::PoseEditSession::stopPreview(){
    if (currentMode != Mode::Previewing) return;
    playing = false;
    endPause = 0.0f;
    currentMode = Mode::Posing;
    workingPose = keyPose(primaryIndex);
    applyCurrent();
    notify();
}

void editor::PoseEditSession::setPreviewTime(float t){
    const float length = previewLength();
    if (!std::isfinite(t)) t = 0.0f;
    time = std::clamp(t, 0.0f, std::max(length, 0.0f));
    endPause = 0.0f;
    if (currentMode == Mode::Previewing){
        applyCurrent();
    }
    notify();
}

float editor::PoseEditSession::previewTime() const { return time; }
bool editor::PoseEditSession::previewPlaying() const { return playing && currentMode == Mode::Previewing; }

void editor::PoseEditSession::setPreviewPlaying(bool play){
    if (play && currentMode != Mode::Previewing){
        startPreview();
        if (currentMode != Mode::Previewing) return;
    }
    if (play && !previewLoops() && time >= previewLength() - 1e-6f){
        time = 0.0f;   // a finished one-shot plays again from the start
    }
    playing = play;
    endPause = 0.0f;
    notify();
}

float editor::PoseEditSession::previewLength(){
    std::shared_ptr<const PoseRealisation> realised = realisation();
    if (!realised || realised->frames.empty()) return 0.0f;
    return static_cast<float>(realised->timeline.length);
}

bool editor::PoseEditSession::previewLoops(){
    return doc && doc->data().settings.loop;
}

int editor::PoseEditSession::previewFrame(){
    std::shared_ptr<const PoseRealisation> realised = realisation();
    if (!realised || realised->frames.size() < 2) return 0;
    const double length = realised->timeline.length;
    const int count = static_cast<int>(realised->frames.size()) - 1;
    if (length <= 0.0) return 0;
    return std::clamp(static_cast<int>(std::floor(time / length * count + 1e-4)), 0, count);
}

void editor::PoseEditSession::samplePreview(pose::HumanPose& out){
    std::shared_ptr<const PoseRealisation> realised = realisation();
    if (!realised || realised->frames.empty()){
        out = rigPtr ? rigPtr->restPose() : pose::HumanPose();
        return;
    }
    const std::vector<pose::HumanPose>& frames = realised->frames;
    const double length = realised->timeline.length;
    const int count = static_cast<int>(frames.size()) - 1;
    if (count <= 0 || length <= 0.0){
        out = frames.front();
        return;
    }
    const double x = std::clamp(time / length, 0.0, 1.0) * count;
    const int f = std::clamp(static_cast<int>(std::floor(x + 1e-4)), 0, count);
    const bool stepped = doc && doc->data().settings.stepRate > 0.0f;
    if (stepped || f >= count){
        out = frames[f];
        return;
    }
    // between two frames, as the runtime shows them: slerp, hips lerped
    const float blend = static_cast<float>(std::clamp(x - f, 0.0, 1.0));
    const pose::HumanPose& a = frames[f];
    const pose::HumanPose& b = frames[f + 1];
    for (int i = 0; i < pose::HUMAN_BONE_COUNT; i++){
        out.rotations[i] = pose::slerpUnclamped(a.rotations[i], b.rotations[i], blend);
    }
    out.hipsPosition = a.hipsPosition + (b.hipsPosition - a.hipsPosition) * blend;
}

void editor::PoseEditSession::update(float dt){
    if (!bound()) return;
    SceneProject* sceneProject = projectPtr ? projectPtr->getScene(sceneIdValue) : nullptr;
    if (!sceneProject || sceneProject->scene != scenePtr){
        dropBinding();   // the scene went away without telling
        return;
    }
    if (!scenePtr->isEntityCreated(modelEntity) || !scenePtr->findComponent<ModelComponent>(modelEntity)){
        dropBinding();   // the model was deleted: its bones too
        return;
    }
    if (sceneProject->playState != ScenePlayState::STOPPED){
        if (sceneProject->playState == ScenePlayState::SAVING){
            if (applied) suspend();
            return;
        }
        // play mode: the preview stops and the bones are back at rest (play runs its own copy)
        if (applied || currentMode != Mode::Idle){
            restoreBones();
            suspended = false;
            playing = false;
            if (globalEdit){
                globalEdit = false;
                globalEditPose = -1;
            }
            currentMode = Mode::Idle;
            notify();
        }
        return;
    }
    if (projectPtr->getSelectedSceneId() != sceneIdValue && currentMode != Mode::Idle){
        // another scene tab: leave this one as it was
        goIdle();
        return;
    }
    if (!bonesValid()){
        dropBinding();
        return;
    }
    if (suspended){
        suspended = false;
        if (currentMode != Mode::Idle && doc && poseCount() > 0){
            applyCurrent();
        }
    }
    if (currentMode == Mode::Previewing && playing && dt > 0.0f){
        const float length = previewLength();
        if (length <= 0.0f){
            playing = false;
        }else if (previewLoops()){
            time = std::fmod(time + dt, length);
            if (time < 0.0f) time += length;
        }else if (time >= length){
            // one-shot animations replay after a 0.5 s pause
            endPause += dt;
            if (endPause >= 0.5f){
                endPause = 0.0f;
                time = 0.0f;
            }
        }else{
            time = std::min(time + dt, length);
        }
        applyCurrent();
        notify();
    }
}

void editor::PoseEditSession::goIdle(){
    const bool changed = currentMode != Mode::Idle || applied;
    if (globalEdit){
        globalEdit = false;
        globalEditPose = -1;
    }
    restoreBones();
    suspended = false;
    playing = false;
    currentMode = Mode::Idle;
    if (changed) notify();
}

void editor::PoseEditSession::suspend(){
    if (!applied) return;
    restoreBones();
    suspended = currentMode != Mode::Idle;
}

// ---- working pose ----

const pose::HumanPose& editor::PoseEditSession::working() const { return workingPose; }

void editor::PoseEditSession::setWorking(const pose::HumanPose& pose){
    workingPose = pose;
    if (currentMode == Mode::Posing && !suspended){
        applyPose(workingPose);
    }
    notify();
}

bool editor::PoseEditSession::commitWorking(const std::string& undoName, const std::string& mergeKey){
    if (!doc || !rigPtr || primaryIndex < 0 || primaryIndex >= poseCount()){
        return false;
    }
    pose::PoseAnimationData data = doc->data();
    pose::PoseKey& key = data.poses[primaryIndex];
    pose::BodyPose body = rigPtr->toBodyPose(workingPose);
    if (data.settings.humanize.jointLimits){
        pose::clampToLimits(body);
    }
    key.body = body;
    if (!key.grounded){
        // the height the working pose has: its lowest point above the floor
        const float lowest = static_cast<float>(rigPtr->lowestPointPrecise(workingPose, rigPtr->skin()));
        const float scale = std::max(rigPtr->humanScale(), 1e-6f);
        key.lift = std::max(0.0f, (lowest - rigPtr->floorHeight()) / scale);
    }
    // grounding happens when the key is realised (the document notification applies it)
    return commitData(data, undoName, mergeKey, nullptr, -1, nullptr, true);
}

editor::PoseEditSession::OnionSettings& editor::PoseEditSession::onion(){ return onionSettings; }
editor::PoseEditSession::ViewSettings& editor::PoseEditSession::view(){ return viewSettings; }

// ---- Global Edit ----

bool editor::PoseEditSession::globalEditActive() const { return globalEdit; }
const pose::HumanPose& editor::PoseEditSession::globalEditBaseline() const { return globalBaseline; }

void editor::PoseEditSession::beginGlobalEdit(){
    if (!doc || primaryIndex < 0 || primaryIndex >= poseCount()) return;
    if (currentMode != Mode::Posing){
        startPosing();
        if (currentMode != Mode::Posing) return;
    }
    globalEdit = true;
    globalEditPose = primaryIndex;
    globalBaseline = keyPose(primaryIndex);
    globalBaselineBody = doc->data().poses[primaryIndex].body;
    notify();
}

int editor::PoseEditSession::globalEditChangedSlots(){
    if (!globalEdit || !doc || globalEditPose < 0 || globalEditPose >= poseCount()) return 0;
    pose::BodyPose probe = globalBaselineBody;
    return poseui::applyGlobalEditDelta(globalBaselineBody, doc->data().poses[globalEditPose].body, probe);
}

bool editor::PoseEditSession::applyGlobalEdit(const std::vector<int>& targets){
    if (!globalEdit || !doc || globalEditPose < 0 || globalEditPose >= poseCount()) return false;
    pose::PoseAnimationData data = doc->data();
    const pose::BodyPose current = data.poses[globalEditPose].body;
    std::vector<int> poses;
    if (targets.empty()){
        for (int i = 0; i < poseCount(); i++){
            if (i != globalEditPose) poses.push_back(i);
        }
    }else{
        for (int i : targets){
            if (i >= 0 && i < poseCount() && i != globalEditPose &&
                std::find(poses.begin(), poses.end(), i) == poses.end()){
                poses.push_back(i);
            }
        }
    }
    if (poses.empty()) return false;
    pose::BodyPose probe = globalBaselineBody;
    if (poseui::applyGlobalEditDelta(globalBaselineBody, current, probe) == 0){
        return false;   // nothing changed yet
    }
    for (int i : poses){
        pose::BodyPose& body = data.poses[i].body;
        poseui::applyGlobalEditDelta(globalBaselineBody, current, body);
        if (data.settings.humanize.jointLimits){
            pose::clampToLimits(body);
        }
    }
    globalEdit = false;
    globalEditPose = -1;
    // one undo step; grounding of grounded keys happens when they are realised
    if (!commitData(data, "Global edit", "", nullptr, -1, nullptr, true)){
        return false;
    }
    notify();
    return true;
}

void editor::PoseEditSession::cancelGlobalEdit(){
    if (!globalEdit) return;
    globalEdit = false;
    const int index = globalEditPose;
    globalEditPose = -1;
    if (doc && doc->exists() && index >= 0 && index < poseCount() && !sameBody(doc->data().poses[index].body, globalBaselineBody)){
        // the edited pose goes back to the baseline (an undoable step of its own)
        pose::PoseAnimationData data = doc->data();
        data.poses[index].body = globalBaselineBody;
        doc->commit(data, "Cancel global edit", "", nullptr, nullptr);
    }
    notify();
}

// ---- bones ----

bool editor::PoseEditSession::bonesValid() const{
    if (!scenePtr || !rigInfoPtr) return false;
    for (Entity entity : rigInfoPtr->boneEntities){
        if (entity != NULL_ENTITY && (!scenePtr->isEntityCreated(entity) || !scenePtr->findComponent<Transform>(entity))){
            return false;
        }
    }
    return true;
}

void editor::PoseEditSession::applyCurrent(){
    if (!bound()) return;
    if (currentMode == Mode::Previewing){
        pose::HumanPose frame = rigPtr->restPose();
        samplePreview(frame);
        applyPose(frame);
    }else if (currentMode == Mode::Posing){
        applyPose(workingPose);
    }
}

void editor::PoseEditSession::applyPose(const pose::HumanPose& pose){
    if (!bound() || !bonesValid()) return;
    const std::vector<Entity>& entities = rigInfoPtr->boneEntities;
    const pose::Skeleton& skeleton = rigPtr->skeleton();
    if (!applied){
        // the bones as the scene has them, put back by restoreBones()
        savedBones.clear();
        for (Entity entity : entities){
            if (entity == NULL_ENTITY) continue;
            const Transform& transform = scenePtr->getComponent<Transform>(entity);
            savedBones.push_back({entity, transform.position, transform.rotation, transform.scale});
        }
        applied = true;
    }
    // every skeleton bone: slots from the pose, the others at rest (as the rig's FK has them)
    const std::vector<Matrix4>& bridges = rigInfoPtr->bridges;
    for (size_t i = 0; i < skeleton.bones.size() && i < entities.size(); i++){
        if (entities[i] == NULL_ENTITY) continue;
        const pose::SkeletonBone& rest = skeleton.bones[i];
        Transform& transform = scenePtr->getComponent<Transform>(entities[i]);
        const int slot = rigPtr->slotOfBone(static_cast<int>(i));
        Vector3 position = slot == pose::boneIndex(pose::HumanBone::Hips) ? pose.hipsPosition : rest.position;
        Quaternion rotation = slot >= 0 ? pose.rotations[slot] : rest.rotation;
        Vector3 scale = rest.scale;
        if (i < bridges.size() && !(bridges[i] == Matrix4())){
            // the skeleton folds non-bone ancestors into this bone (skeleton local = bridge *
            // Transform local): the Transform gets them taken off again
            const Matrix4 local = bridges[i].inverse() * Matrix4::translateMatrix(position) * rotation.getRotationMatrix() *
                                  Matrix4::scaleMatrix(scale);
            local.decompose(position, scale, rotation);
        }
        transform.position = position;
        transform.rotation = rotation;
        transform.scale = scale;
        transform.needUpdate = true;
    }
    shown = pose;
    if (SceneProject* sceneProject = projectPtr->getScene(sceneIdValue)){
        sceneProject->needUpdateRender = true;
    }
}

void editor::PoseEditSession::restoreBones(){
    if (!applied) return;
    applied = false;
    if (scenePtr){
        for (const SavedBone& saved : savedBones){
            if (!scenePtr->isEntityCreated(saved.entity)) continue;
            Transform* transform = scenePtr->findComponent<Transform>(saved.entity);
            if (!transform) continue;
            transform->position = saved.position;
            transform->rotation = saved.rotation;
            transform->scale = saved.scale;
            transform->needUpdate = true;
        }
        if (SceneProject* sceneProject = projectPtr ? projectPtr->getScene(sceneIdValue) : nullptr){
            sceneProject->needUpdateRender = true;
        }
    }
    savedBones.clear();
    shown = rigPtr ? rigPtr->restPose() : pose::HumanPose();
}
