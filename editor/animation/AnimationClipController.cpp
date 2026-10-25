// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationClipController.h"

#include "Out.h"
#include "Project.h"
#include "Scene.h"
#include "animation/AnimationClipAppHooks.h"
#include "animation/AnimationClipIO.h"
#include "animation/ClipBinding.h"
#include "animation/KeyframeSampling.h"
#include "component/AnimationPlayerComponent.h"
#include "component/LatticeComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "lattice/LatticeSolver.h"
#include "math/AABB.h"
#include "math/Matrix4.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/RenderSystem.h"
#include "util/HybridArray.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

// The derived state of the previewed hierarchy when the preview started: what the frames
// compute from the local values the preview writes (world matrices, skinning matrices and
// bounds), so a scene saved right after the restore is the scene as it was (PoseEditSession
// does the same for its bones)
struct AnimationClipController::Snapshot{
    struct SavedTransform{
        Entity entity = NULL_ENTITY;
        Entity parent = NULL_ENTITY;
        Vector3 position;
        Quaternion rotation;
        Vector3 scale;
        Vector3 worldPosition;
        Quaternion worldRotation;
        Vector3 worldScale;
        Matrix4 localMatrix;
        Matrix4 modelMatrix;
        Matrix4 normalMatrix;
        Matrix4 parentMatrix;           // the parent's modelMatrix then
    };
    struct SavedMesh{
        Entity entity = NULL_ENTITY;
        HybridArray<Matrix4, MAX_BONES> bonesMatrix;
        AABB skinnedAABB;
        AABB worldAABB;
        bool needUpdateAABB = false;
        Matrix4 modelMatrix;
    };
    // parents before their children
    std::vector<SavedTransform> transforms;
    std::vector<SavedMesh> meshes;
};

namespace{

    const char* const PREVIEW_OWNER_NAME = "Animation window";

    bool fail(std::string* error, const std::string& message){
        if (error) *error = message;
        return false;
    }

    std::map<Project*, std::unique_ptr<AnimationClipController>>& controllers(){
        static std::map<Project*, std::unique_ptr<AnimationClipController>> map;
        return map;
    }

    // the App's hooks (animation clips 9.R4-R7): every live controller (one per project)
    struct AppHooksInstaller{
        AppHooksInstaller(){
            AnimationClipAppHooks& hooks = animationClipAppHooks();
            hooks.stopTransientPreviews = [](){
                for (auto& entry : controllers()) entry.second->stopTransientPreviews();
            };
            hooks.ownsUndo = [](const ClipUndoFocus& focus){
                for (auto& entry : controllers()){
                    if (entry.second->ownsUndo(focus)) return true;
                }
                return false;
            };
            hooks.takesKeys = [](){
                for (auto& entry : controllers()){
                    if (entry.second->takesKeys()) return true;
                }
                return false;
            };
            hooks.sceneClosed = [](uint32_t sceneId){
                for (auto& entry : controllers()) entry.second->sceneClosed(sceneId);
                PreviewOwner::releaseScene(sceneId);
            };
            hooks.projectClosed = [](){
                std::vector<Project*> projects;
                for (auto& entry : controllers()) projects.push_back(entry.first);
                for (Project* project : projects) AnimationClipController::release(project);
            };
        }
    };
    const AppHooksInstaller appHooksInstaller;

    bool finite(float value){
        return std::isfinite(value);
    }

    // the key of a track nearest `time` within `tolerance`, -1 when none
    int nearestKey(const ClipTrackData& track, float time, float tolerance){
        int best = -1;
        float bestDistance = tolerance;
        for (size_t k = 0; k < track.keys.size(); k++){
            const float distance = std::fabs(track.keys[k].time - time);
            if (distance <= bestDistance){
                best = (int)k;
                bestDistance = distance;
            }
        }
        return best;
    }

    // a key holding `value` at `time` (an existing key there takes the value, keeping its ease
    // and tangents' width; a new key splits the segment it falls in and keeps its ease)
    void setKeyAt(ClipTrackData& track, float time, const std::vector<float>& value){
        for (ClipKey& key : track.keys){
            if (std::fabs(key.time - time) <= anim::ANIM_TIME_EPSILON){
                key.value = value;
                if (key.hasTangents() && key.inTangent.size() != value.size()){
                    key.inTangent.assign(value.size(), 0.0f);
                    key.outTangent.assign(value.size(), 0.0f);
                }
                return;
            }
        }
        ClipKey key;
        key.time = time;
        key.value = value;
        auto it = std::upper_bound(track.keys.begin(), track.keys.end(), time,
                                   [](float t, const ClipKey& other){ return t < other.time; });
        if (it != track.keys.begin()) key.ease = std::prev(it)->ease;
        track.keys.insert(it, key);
    }

    // a morph value as wide as the track needs: the mesh's weights without trailing zeros (a
    // missing weight is 0), at least the track's width
    std::vector<float> morphValue(const ClipTrackData& track, std::vector<float> value){
        size_t width = 0;
        for (const ClipKey& key : track.keys) width = std::max(width, key.value.size());
        size_t used = value.size();
        while (used > 0 && value[used - 1] == 0.0f) used--;
        width = std::max(width, std::max<size_t>(used, 1));
        value.resize(std::min<size_t>(width, MAX_MORPHTARGETS), 0.0f);
        return value;
    }

    // the flat value of a sample (ClipKey::value layout)
    std::vector<float> sampleValue(const anim::TrackSample& sample){
        switch (sample.channel){
            case ClipChannel::Position:
            case ClipChannel::Scale:
                return {sample.vector.x, sample.vector.y, sample.vector.z};
            case ClipChannel::Rotation: {
                std::vector<float> out(4);
                quaternionToXYZW(sample.rotation, out.data());
                return out;
            }
            case ClipChannel::Morph:
                return sample.weights;
            case ClipChannel::Lattice: {
                std::vector<float> out;
                for (const Vector3& offset : sample.offsets) out.insert(out.end(), {offset.x, offset.y, offset.z});
                return out;
            }
            case ClipChannel::Property:
                break;
        }
        return {};
    }

    bool sameValue(ClipChannel channel, const std::vector<float>& a, const std::vector<float>& b){
        const float tolerance = 1e-4f;
        if (channel == ClipChannel::Rotation && a.size() == 4 && b.size() == 4){
            // q and -q are one rotation
            const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
            return std::fabs(std::fabs(dot) - 1.0f) <= tolerance;
        }
        const size_t width = std::max(a.size(), b.size());
        for (size_t i = 0; i < width; i++){
            const float x = i < a.size() ? a[i] : 0.0f;
            const float y = i < b.size() ? b[i] : 0.0f;
            if (std::fabs(x - y) > tolerance) return false;
        }
        return true;
    }

    // the rest value of one channel in ClipKey::value layout (the whole cage for lattice)
    bool restValue(Scene* scene, Entity entity, const anim::ChannelRest& rest, ClipChannel channel, std::vector<float>& out){
        out.clear();
        switch (channel){
            case ClipChannel::Position:
                if (!rest.hasTransform) return false;
                out = {rest.position.x, rest.position.y, rest.position.z};
                return true;
            case ClipChannel::Scale:
                if (!rest.hasTransform) return false;
                out = {rest.scale.x, rest.scale.y, rest.scale.z};
                return true;
            case ClipChannel::Rotation:
                if (!rest.hasTransform) return false;
                out.resize(4);
                quaternionToXYZW(rest.rotation, out.data());
                return true;
            case ClipChannel::Morph:
                if (!rest.hasMorph) return false;
                out = rest.morphWeights;
                return true;
            case ClipChannel::Lattice: {
                if (!rest.hasLattice) return false;
                const LatticeComponent* latticecomp = scene->findComponent<LatticeComponent>(entity);
                if (!latticecomp) return false;
                const size_t count = (size_t)lattice::pointCount(latticecomp->resolutionX, latticecomp->resolutionY, latticecomp->resolutionZ);
                for (size_t p = 0; p < count; p++){
                    const Vector3 offset = p < rest.latticeOffsets.size() ? rest.latticeOffsets[p] : Vector3(0, 0, 0);
                    out.insert(out.end(), {offset.x, offset.y, offset.z});
                }
                return true;
            }
            case ClipChannel::Property:
                break;
        }
        return false;
    }

    // the scene value of the channel's component part of `rest` (taken when it has none yet,
    // or `update`: taken again)
    void takeRest(Scene* scene, Entity entity, anim::ChannelRest& rest, ClipChannel channel, bool update){
        switch (channel){
            case ClipChannel::Position:
            case ClipChannel::Rotation:
            case ClipChannel::Scale: {
                const Transform* transform = scene->findComponent<Transform>(entity);
                if (!transform) return;
                if (!rest.hasTransform){
                    rest.hasTransform = true;
                    rest.position = transform->position;
                    rest.rotation = transform->rotation;
                    rest.scale = transform->scale;
                }else if (update){
                    if (channel == ClipChannel::Position) rest.position = transform->position;
                    if (channel == ClipChannel::Rotation) rest.rotation = transform->rotation;
                    if (channel == ClipChannel::Scale) rest.scale = transform->scale;
                }
                return;
            }
            case ClipChannel::Morph: {
                const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
                if (!mesh || (rest.hasMorph && !update)) return;
                rest.hasMorph = true;
                rest.morphWeights.assign(mesh->morphWeights, mesh->morphWeights + MAX_MORPHTARGETS);
                return;
            }
            case ClipChannel::Lattice: {
                const LatticeComponent* latticecomp = scene->findComponent<LatticeComponent>(entity);
                if (!latticecomp || (rest.hasLattice && !update)) return;
                rest.hasLattice = true;
                const size_t count = (size_t)lattice::pointCount(latticecomp->resolutionX, latticecomp->resolutionY, latticecomp->resolutionZ);
                rest.latticeOffsets.assign(count, Vector3(0, 0, 0));
                for (size_t p = 0; p < count && p < latticecomp->offsets.size(); p++) rest.latticeOffsets[p] = latticecomp->offsets[p];
                return;
            }
            case ClipChannel::Property:
                return;
        }
    }

    bool hasRestFor(const anim::ChannelRest& rest, ClipChannel channel){
        switch (channel){
            case ClipChannel::Position:
            case ClipChannel::Rotation:
            case ClipChannel::Scale:
                return rest.hasTransform;
            case ClipChannel::Morph:
                return rest.hasMorph;
            case ClipChannel::Lattice:
                return rest.hasLattice;
            case ClipChannel::Property:
                break;
        }
        return false;
    }

    // the hierarchy under `root` (root first, parents before children), one pass over the
    // Transforms
    std::vector<Entity> subtreeOf(Scene* scene, Entity root){
        std::vector<Entity> out;
        if (!scene || root == NULL_ENTITY || !scene->isEntityCreated(root)) return out;
        std::unordered_map<Entity, std::vector<Entity>> children;
        auto transforms = scene->getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++){
            const Transform& transform = transforms->getComponentFromIndex(i);
            if (transform.parent != NULL_ENTITY) children[transform.parent].push_back(transforms->getEntity(i));
        }
        std::vector<Entity> stack = {root};
        std::set<Entity> seen;
        while (!stack.empty()){
            const Entity entity = stack.back();
            stack.pop_back();
            if (!seen.insert(entity).second) continue;
            out.push_back(entity);
            auto it = children.find(entity);
            if (it == children.end()) continue;
            for (auto child = it->second.rbegin(); child != it->second.rend(); ++child) stack.push_back(*child);
        }
        return out;
    }

    std::string readOnlyError(const AnimationClipDocument& document){
        return "'" + document.path() + "' is a model's animation (read-only): Extract to .anim to edit it";
    }
}

// ---- construction ----

AnimationClipController::AnimationClipController(Project* project): project(project){
    // scene commands, undo and redo writing a previewed channel: the command sees (and edits)
    // the rest, which becomes the snapshot (9.R6)
    beforeWriteListener = ClipRecordHooks::addBeforeChannelWrite([this](uint32_t id, Entity entity, ClipChannel channel){
        beforeSceneWrite(id, entity, channel);
    });
    afterWriteListener = ClipRecordHooks::addAfterChannelWrite([this](uint32_t id, Entity entity, ClipChannel channel){
        afterSceneWrite(id, entity, channel);
    });
}

AnimationClipController::~AnimationClipController(){
    // never touches scenes (release() restored them while they existed)
    ClipRecordHooks::removeChannelWriteListener(beforeWriteListener);
    ClipRecordHooks::removeChannelWriteListener(afterWriteListener);
    ClipRecordHooks::removeRecordListener(recordListener);
    PreviewOwner::release(ownerToken);
    if (document && documentListener) document->removeListener(documentListener);
}

AnimationClipController& AnimationClipController::shared(Project* project){
    std::unique_ptr<AnimationClipController>& controller = controllers()[project];
    if (!controller) controller.reset(new AnimationClipController(project));
    return *controller;
}

void AnimationClipController::release(Project* project){
    auto it = controllers().find(project);
    if (it == controllers().end()) return;
    std::unique_ptr<AnimationClipController> controller = std::move(it->second);
    controllers().erase(it);
    controller->stopPreview();
    if (controller->stopPending){
        // a thread is saving the scene: nothing is written under it (the project closes)
        controller->endPreview(false);
    }
    controller->closeClip();
}

// ---- helpers ----

SceneProject* AnimationClipController::sceneProjectOf(uint32_t id) const{
    if (!project || id == 0) return nullptr;
    SceneProject* sceneProject = project->getScene(id);
    return sceneProject && sceneProject->scene ? sceneProject : nullptr;
}

Entity AnimationClipController::findPlayerRoot(EntityRegistry* registry, Entity entity){
    if (!registry || entity == NULL_ENTITY || !registry->isEntityCreated(entity)) return NULL_ENTITY;
    Entity current = entity;
    for (int depth = 0; depth < 100000 && current != NULL_ENTITY; depth++){
        if (!registry->isEntityCreated(current)) return NULL_ENTITY;
        if (registry->findComponent<AnimationPlayerComponent>(current)) return current;
        const Transform* transform = registry->findComponent<Transform>(current);
        current = transform ? transform->parent : NULL_ENTITY;
    }
    return NULL_ENTITY;
}

// ---- binding and clip ----

bool AnimationClipController::bindRoot(uint32_t sceneId, Entity entity, std::string* error){
    if (entity == NULL_ENTITY){
        stopPreview();
        if (!stateName.empty()) closeClip();
        this->sceneId = 0;
        root = NULL_ENTITY;
        stateName.clear();
        return true;
    }
    SceneProject* sceneProject = sceneProjectOf(sceneId);
    if (!sceneProject || !sceneProject->scene->isEntityCreated(entity)){
        return fail(error, "no such GameObject in the scene");
    }
    Entity target = findPlayerRoot(sceneProject->scene, entity);
    if (target == NULL_ENTITY) target = entity;
    if (sceneId == this->sceneId && target == root) return true;

    // another root: the preview of the old one goes back first
    stopPreview();
    const bool hadState = !stateName.empty();
    this->sceneId = sceneId;
    root = target;
    stateName.clear();
    if (!hasPlayer()){
        // a clip of the old root's player is not this object's
        if (hadState) closeClip();
        return true;
    }
    if (document){
        stateName = stateOfDocument();
        if (!stateName.empty()) return true;
    }
    // the player's default clip (the first one when it names none)
    const AnimationPlayerComponent& player = sceneProject->scene->getComponent<AnimationPlayerComponent>(root);
    const std::vector<std::string> names = anim::playerStateNames(player.clips);
    if (names.empty()){
        if (hadState) closeClip();
        return true;
    }
    std::string state = names.front();
    if (!player.defaultClip.empty() && std::find(names.begin(), names.end(), player.defaultClip) != names.end()){
        state = player.defaultClip;
    }
    std::string openError;
    if (!openState(state, &openError)){
        // a missing clip file: the window shows the state without a document
        if (hadState) closeClip();
        stateName = state;
    }
    return true;
}

uint32_t AnimationClipController::getSceneId() const{
    return sceneId;
}

Entity AnimationClipController::getRoot() const{
    return root;
}

bool AnimationClipController::hasPlayer() const{
    SceneProject* sceneProject = sceneProjectOf(sceneId);
    return sceneProject && root != NULL_ENTITY && sceneProject->scene->isEntityCreated(root) &&
           sceneProject->scene->findComponent<AnimationPlayerComponent>(root) != nullptr;
}

std::vector<std::string> AnimationClipController::getStateNames() const{
    if (!hasPlayer()) return {};
    const AnimationPlayerComponent& player = sceneProjectOf(sceneId)->scene->getComponent<AnimationPlayerComponent>(root);
    return anim::playerStateNames(player.clips);
}

std::string AnimationClipController::stateOfDocument() const{
    if (!document || !hasPlayer()) return "";
    const AnimationPlayerComponent& player = sceneProjectOf(sceneId)->scene->getComponent<AnimationPlayerComponent>(root);
    const std::vector<AnimationPlayerClip> entries = player.clips;
    const std::vector<std::string> names = anim::playerStateNames(entries);
    for (size_t i = 0; i < entries.size() && i < names.size(); i++){
        if (entries[i].clip.empty()) continue;
        // documents are shared per file: the same document is the same clip
        if (AnimationClipDocument::open(project, entries[i].clip, nullptr) == document) return names[i];
    }
    return "";
}

void AnimationClipController::setDocument(const std::shared_ptr<AnimationClipDocument>& next){
    if (next == document) return;
    endKeyDrag();
    if (document && documentListener) document->removeListener(documentListener);
    documentListener = 0;
    document = next;
    clipPath = document ? document->path() : std::string();
    unresolved.clear();
    if (document){
        documentListener = document->addListener([this](AnimationClipDocument&, AnimationClipDocument::Change change){
            if (change == AnimationClipDocument::Change::Removed){
                // an undone creation or a deletion: nothing to preview any more
                stopPreview();
                return;
            }
            // an edit, its undo / redo or a reload: the preview shows the new revision
            if (previewing) apply();
        });
    }
    if (previewing) apply();
}

bool AnimationClipController::openClip(const std::string& path, std::string* error){
    if (!project) return fail(error, "no project is open");
    std::shared_ptr<AnimationClipDocument> next = AnimationClipDocument::open(project, path, error);
    if (!next) return false;
    setDocument(next);
    stateName = stateOfDocument();
    return true;
}

bool AnimationClipController::openState(const std::string& name, std::string* error){
    if (!hasPlayer()) return fail(error, "the bound GameObject has no Animation Player");
    const AnimationPlayerComponent& player = sceneProjectOf(sceneId)->scene->getComponent<AnimationPlayerComponent>(root);
    const std::vector<AnimationPlayerClip> entries = player.clips;
    const std::vector<std::string> names = anim::playerStateNames(entries);
    auto it = std::find(names.begin(), names.end(), name);
    if (it == names.end()) return fail(error, "the Animation Player has no clip named '" + name + "'");
    const AnimationPlayerClip& entry = entries[(size_t)(it - names.begin())];
    if (entry.clip.empty()) return fail(error, "the Animation Player's clip '" + name + "' has no file");
    std::shared_ptr<AnimationClipDocument> next = AnimationClipDocument::open(project, entry.clip, error);
    if (!next) return false;
    setDocument(next);
    stateName = name;
    return true;
}

void AnimationClipController::closeClip(){
    stopPreview();
    setRecording(false);
    setDocument(nullptr);
    stateName.clear();
}

std::shared_ptr<AnimationClipDocument> AnimationClipController::getDocument() const{
    return document;
}

const std::string& AnimationClipController::getClipPath() const{
    return clipPath;
}

const std::string& AnimationClipController::getStateName() const{
    return stateName;
}

bool AnimationClipController::isReadOnly() const{
    return document && document->isReadOnly();
}

std::string AnimationClipController::getPreviewModel() const{
    return document ? document->data().previewModel : std::string();
}

ClipValidationReport AnimationClipController::getBindings() const{
    if (!document) return ClipValidationReport();
    SceneProject* sceneProject = nullptr;
    const Entity target = valueRoot(&sceneProject);
    if (target == NULL_ENTITY || !sceneProject) return ClipValidation::validate(document->data(), nullptr, NULL_ENTITY);
    return ClipValidation::validateInScene(project, sceneProject, document->data(), target);
}

Entity AnimationClipController::findPreviewInstance(SceneProject* sceneProject) const{
    if (!project || !document || !sceneProject) return NULL_ENTITY;
    const std::string& model = document->data().previewModel;
    if (model.empty()) return NULL_ENTITY;
    Scene* scene = sceneProject->scene;
    const fs::path target = project->resolveAssetPath(fs::u8path(model)).lexically_normal();
    auto matches = [&](Entity entity){
        if (!scene->isEntityCreated(entity)) return false;
        if (const ModelComponent* component = scene->findComponent<ModelComponent>(entity)){
            if (!component->filename.empty() &&
                project->resolveAssetPath(fs::u8path(component->filename)).lexically_normal() == target) return true;
        }
        PrefabObjectRef ref;
        if (project->isPrefabInstanceRoot(sceneProject->id, entity) && project->findPrefabObject(sceneProject->id, entity, ref) &&
            !ref.path.empty()){
            const fs::path prefab = ref.path.is_absolute() ? ref.path : project->getProjectPath() / ref.path;
            if (prefab.lexically_normal() == target) return true;
        }
        return false;
    };
    for (Entity entity : sceneProject->selectedEntities){
        if (matches(entity)) return entity;
    }
    auto transforms = scene->getComponentArray<Transform>();
    for (size_t i = 0; i < transforms->size(); i++){
        const Entity entity = transforms->getEntity(i);
        if (matches(entity)) return entity;
    }
    return NULL_ENTITY;
}

// ---- preview and transport ----

bool AnimationClipController::startPreview(std::string* error){
    if (stopPending) return fail(error, "the scene is being saved");
    if (previewing) return true;
    if (!project) return fail(error, "no project is open");
    if (!document) return fail(error, "no clip is open");
    uint32_t targetScene = sceneId;
    Entity target = root;
    SceneProject* sceneProject = sceneProjectOf(targetScene);
    if (target == NULL_ENTITY || !sceneProject || !sceneProject->scene->isEntityCreated(target)){
        // target-less: the clip's preview model, as instanced in the selected scene
        targetScene = project->getSelectedSceneId();
        sceneProject = sceneProjectOf(targetScene);
        target = findPreviewInstance(sceneProject);
        if (target == NULL_ENTITY){
            const std::string& model = document->data().previewModel;
            return fail(error, model.empty()
                ? "nothing to preview on: select a GameObject, or set the clip's Preview on model (preview.model)"
                : "nothing to preview on: no instance of '" + model + "' (the clip's preview model) in the open scene");
        }
    }
    if (sceneProject->playState != ScenePlayState::STOPPED){
        return fail(error, "the scene is playing or being saved: previews run in edit mode");
    }

    // one edit-mode pose owner per root: the Pose window, the Inspector's action preview or
    // another preview of this hierarchy restore their snapshots now (9.R7)
    ownerToken = PreviewOwner::acquire(targetScene, target, sceneProject->scene, PREVIEW_OWNER_NAME, [this](){ stopPreview(); });

    previewSceneId = targetScene;
    previewRoot = target;
    session.reset();
    session.root = target;
    takeSnapshot(sceneProject);
    previewing = true;
    stopPending = false;
    apply();
    return true;
}

void AnimationClipController::stopPreview(){
    if (!previewing){
        PreviewOwner::release(ownerToken);
        ownerToken = 0;
        return;
    }
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    if (sceneProject && sceneProject->playState == ScenePlayState::SAVING){
        // a thread is writing the scene (it began after stopTransientPreviews, so it writes
        // the restored scene only if this preview wrote nothing since): finished by update()
        stopPending = true;
        playing = false;
        return;
    }
    endPreview(sceneProject != nullptr);
}

void AnimationClipController::endPreview(bool write){
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    if (write && sceneProject){
        sceneProject->scene->getSystem<ActionSystem>()->restorePlayerPreview(session);
        restoreSnapshot(sceneProject);
        sceneProject->needUpdateRender = true;
    }
    session.reset();
    snapshot.reset();
    previewing = false;
    playing = false;
    stopPending = false;
    needsApply = false;
    unresolved.clear();
    previewSceneId = 0;
    previewRoot = NULL_ENTITY;
    // Record keys into the preview: it ends with it (Unity)
    if (recording) setRecording(false);
    PreviewOwner::release(ownerToken);
    ownerToken = 0;
}

bool AnimationClipController::isPreviewing() const{
    return previewing && !stopPending;
}

void AnimationClipController::play(){
    if (!isPreviewing() && !startPreview(nullptr)) return;
    playing = true;
}

void AnimationClipController::pause(){
    playing = false;
}

bool AnimationClipController::isPlaying() const{
    return playing;
}

void AnimationClipController::setPlaybackSpeed(float speed){
    if (finite(speed)) playbackSpeed = speed;
}

float AnimationClipController::getPlaybackSpeed() const{
    return playbackSpeed;
}

void AnimationClipController::setTime(float value){
    if (!finite(value)) return;
    time = std::max(0.0f, value);
}

void AnimationClipController::seek(float value){
    setTime(value);
    if (!isPreviewing()){
        startPreview(nullptr);
        return;
    }
    apply();
}

float AnimationClipController::getTime() const{
    return time;
}

float AnimationClipController::getLength() const{
    return document ? document->data().effectiveLength() : 0.0f;
}

void AnimationClipController::update(double dt){
    if (stopPending){
        SceneProject* sceneProject = sceneProjectOf(previewSceneId);
        if (!sceneProject || sceneProject->playState != ScenePlayState::SAVING) endPreview(sceneProject != nullptr);
        return;
    }
    // a deleted bound root (or its scene gone) unbinds; a clip of its player closes
    if (root != NULL_ENTITY){
        SceneProject* bound = sceneProjectOf(sceneId);
        if (!bound || !bound->scene->isEntityCreated(root)){
            if (!stateName.empty()) closeClip();
            root = NULL_ENTITY;
            sceneId = 0;
            stateName.clear();
        }
    }
    if (!previewing) return;
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    if (!sceneProject){
        endPreview(false);
        return;
    }
    // play mode, another scene shown, the previewed root deleted: the preview ends (restored)
    if ((sceneProject->playState != ScenePlayState::STOPPED && sceneProject->playState != ScenePlayState::SAVING) ||
        (project && project->getSelectedSceneId() != previewSceneId) || !sceneProject->scene->isEntityCreated(previewRoot)){
        stopPreview();
        return;
    }
    if (playing && finite((float)dt)){
        // the window's playback loops the clip (Unity), whatever its wrap mode
        const float length = getLength();
        time += (float)dt * playbackSpeed;
        if (length <= 0.0f){
            time = 0.0f;
        }else if (time > length){
            time = std::fmod(time, length);
        }else if (time < 0.0f){
            time = length + std::fmod(time, length);
        }
        apply();
    }else if (needsApply){
        apply();
    }
}

bool AnimationClipController::apply(){
    needsApply = false;
    if (!previewing || stopPending || !document) return false;
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    if (!sceneProject || sceneProject->playState != ScenePlayState::STOPPED) return false;
    Scene* scene = sceneProject->scene;
    if (!scene->isEntityCreated(previewRoot)) return false;

    ActionSystem::PlayerPreview preview;
    ActionSystem::PlayerPreviewState state;
    state.clip = document->clip();
    state.time = time;
    // the playhead is a clip time: past the end shows the end (playback loops in update)
    state.wrap = ClipWrapMode::ClampForever;
    preview.states.push_back(state);
    ActionSystem* actions = scene->getSystem<ActionSystem>().get();
    ActionSystem::PlayerPreviewResult result = actions->previewPlayer(previewRoot, preview, session);

    // channels the clip no longer animates (a track removed or retargeted) show their rest
    for (const std::pair<Entity, ClipChannel>& entry : session.written){
        if (std::find(result.channels.begin(), result.channels.end(), entry) == result.channels.end()){
            writeRest(entry.first, entry.second);
        }
    }
    unresolved = result.unresolved;
    sceneProject->needUpdateRender = true;
    return result.ready;
}

void AnimationClipController::writeRest(Entity entity, ClipChannel channel){
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    if (!sceneProject || !sceneProject->scene->isEntityCreated(entity)) return;
    auto it = session.rest.find(entity);
    if (it == session.rest.end()) return;
    std::vector<float> value;
    if (!restValue(sceneProject->scene, entity, it->second, channel, value)) return;
    anim::writeChannelValue(sceneProject->scene, entity, channel, {}, value);
}

void AnimationClipController::takeSnapshot(SceneProject* sceneProject){
    snapshot.reset(new Snapshot());
    Scene* scene = sceneProject->scene;
    for (Entity entity : subtreeOf(scene, previewRoot)){
        if (const Transform* transform = scene->findComponent<Transform>(entity)){
            Snapshot::SavedTransform saved;
            saved.entity = entity;
            saved.parent = transform->parent;
            saved.position = transform->position;
            saved.rotation = transform->rotation;
            saved.scale = transform->scale;
            saved.worldPosition = transform->worldPosition;
            saved.worldRotation = transform->worldRotation;
            saved.worldScale = transform->worldScale;
            saved.localMatrix = transform->localMatrix;
            saved.modelMatrix = transform->modelMatrix;
            saved.normalMatrix = transform->normalMatrix;
            const Transform* parent = transform->parent != NULL_ENTITY ? scene->findComponent<Transform>(transform->parent) : nullptr;
            if (parent) saved.parentMatrix = parent->modelMatrix;
            snapshot->transforms.push_back(saved);
        }
        if (const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
            Snapshot::SavedMesh saved;
            saved.entity = entity;
            saved.bonesMatrix = mesh->bonesMatrix;
            saved.skinnedAABB = mesh->skinnedAABB;
            saved.worldAABB = mesh->worldAABB;
            saved.needUpdateAABB = mesh->needUpdateAABB;
            const Transform* transform = scene->findComponent<Transform>(entity);
            if (transform) saved.modelMatrix = transform->modelMatrix;
            snapshot->meshes.push_back(saved);
        }
    }
}

void AnimationClipController::restoreSnapshot(SceneProject* sceneProject){
    if (!snapshot) return;
    Scene* scene = sceneProject->scene;
    RenderSystem* render = scene->getSystem<RenderSystem>().get();
    // the local values are the rest again (restorePlayerPreview): what follows from them is
    // the saved state where nothing under it changed, else computed as the next frame would
    bool unchanged = true;
    for (const Snapshot::SavedTransform& saved : snapshot->transforms){
        if (!scene->isEntityCreated(saved.entity)) continue;
        Transform* transform = scene->findComponent<Transform>(saved.entity);
        if (!transform) continue;
        const Transform* parent = transform->parent != NULL_ENTITY ? scene->findComponent<Transform>(transform->parent) : nullptr;
        const bool sameLocal = transform->parent == saved.parent && transform->position == saved.position &&
                               transform->rotation == saved.rotation && transform->scale == saved.scale;
        const bool sameParent = parent ? parent->modelMatrix == saved.parentMatrix : saved.parent == NULL_ENTITY;
        if (sameLocal && sameParent){
            transform->worldPosition = saved.worldPosition;
            transform->worldRotation = saved.worldRotation;
            transform->worldScale = saved.worldScale;
            transform->localMatrix = saved.localMatrix;
            transform->modelMatrix = saved.modelMatrix;
            transform->normalMatrix = saved.normalMatrix;
        }else{
            unchanged = false;
            if (render) render->updateTransform(*transform, saved.entity);
        }
        transform->needUpdate = true;
    }
    for (const Snapshot::SavedMesh& saved : snapshot->meshes){
        if (!scene->isEntityCreated(saved.entity)) continue;
        MeshComponent* mesh = scene->findComponent<MeshComponent>(saved.entity);
        if (!mesh) continue;
        if (unchanged){
            mesh->bonesMatrix = saved.bonesMatrix;
            mesh->skinnedAABB = saved.skinnedAABB;
            mesh->worldAABB = saved.worldAABB;
            mesh->needUpdateAABB = saved.needUpdateAABB;
        }else{
            // the rest changed while previewing: the next frame computes them
            mesh->needUpdateAABB = true;
        }
    }
}

std::vector<AnimationClipController::BoundTransform> AnimationClipController::sampledBoundTransforms(size_t max) const{
    std::vector<BoundTransform> out;
    if (!document) return out;
    SceneProject* sceneProject = nullptr;
    const Entity target = valueRoot(&sceneProject);
    if (target == NULL_ENTITY || !sceneProject) return out;
    Scene* scene = sceneProject->scene;
    std::set<Entity> listed;
    for (const ClipTrackData& track : document->data().tracks){
        if (out.size() >= max) break;
        if (track.channel == ClipChannel::Property) continue;
        const Entity entity = anim::resolvePath(scene, target, track.path);
        if (entity == NULL_ENTITY || !listed.insert(entity).second) continue;
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform) continue;
        BoundTransform bound;
        bound.path = track.path;
        bound.entity = entity;
        bound.position = transform->position;
        bound.rotation = transform->rotation;
        bound.scale = transform->scale;
        out.push_back(bound);
    }
    return out;
}

Entity AnimationClipController::getPreviewRoot() const{
    return isPreviewing() ? previewRoot : NULL_ENTITY;
}

uint32_t AnimationClipController::getPreviewSceneId() const{
    return isPreviewing() ? previewSceneId : 0;
}

const std::vector<std::string>& AnimationClipController::getUnresolvedPaths() const{
    return unresolved;
}

Entity AnimationClipController::valueRoot(SceneProject** sceneProject) const{
    if (previewing){
        SceneProject* previewed = sceneProjectOf(previewSceneId);
        if (previewed && previewed->scene->isEntityCreated(previewRoot)){
            if (sceneProject) *sceneProject = previewed;
            return previewRoot;
        }
    }
    SceneProject* bound = sceneProjectOf(sceneId);
    if (bound && root != NULL_ENTITY && bound->scene->isEntityCreated(root)){
        if (sceneProject) *sceneProject = bound;
        return root;
    }
    if (sceneProject) *sceneProject = nullptr;
    return NULL_ENTITY;
}

// ---- scene writes during a preview (9.R6) ----

void AnimationClipController::beforeSceneWrite(uint32_t id, Entity entity, ClipChannel channel){
    if (!previewing || id != previewSceneId) return;
    // the command reads and edits the rest, not the previewed value
    if (std::find(session.written.begin(), session.written.end(), std::make_pair(entity, channel)) != session.written.end()){
        writeRest(entity, channel);
    }
}

void AnimationClipController::afterSceneWrite(uint32_t id, Entity entity, ClipChannel channel){
    if (!previewing || id != previewSceneId) return;
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    if (!sceneProject) return;
    auto it = session.rest.find(entity);
    if (it == session.rest.end() || !hasRestFor(it->second, channel)) return;
    // the value the command wrote is the new rest; the preview shows again on the next update
    // (not now: the command's other channels are notified after this one)
    takeRest(sceneProject->scene, entity, it->second, channel, true);
    needsApply = true;
}

// ---- keying ----

bool AnimationClipController::editable(std::string* error) const{
    if (!project) return fail(error, "no project is open");
    if (!document) return fail(error, "no clip is open");
    if (document->isReadOnly()) return fail(error, readOnlyError(*document));
    if (!document->exists()) return fail(error, "'" + document->path() + "' does not exist");
    return true;
}

bool AnimationClipController::commitEdit(const ClipData& data, const std::string& undoName, const std::string& mergeKey, std::string* error){
    if (!editable(error)) return false;
    // keep the document across the commit: its listener may run stopPreview()
    std::shared_ptr<AnimationClipDocument> edited = document;
    if (!edited->commit(data, undoName, mergeKey, error)) return false;
    // the preview shows the new revision through the document's listener
    lastActionWasClipEdit = true;
    return true;
}

bool AnimationClipController::currentTrackValue(const ClipTrackData& track, std::vector<float>& out) const{
    out.clear();
    SceneProject* sceneProject = nullptr;
    const Entity target = valueRoot(&sceneProject);
    if (target == NULL_ENTITY || !sceneProject || track.channel == ClipChannel::Property) return false;
    Scene* scene = sceneProject->scene;
    const Entity entity = anim::resolvePath(scene, target, track.path);
    if (entity == NULL_ENTITY) return false;
    if (!anim::readChannelValue(scene, entity, track.channel, track.points, out)) return false;
    if (track.channel == ClipChannel::Morph) out = morphValue(track, out);
    if (!track.relative) return true;

    // relative tracks key the offset from the rest (the preview's snapshot; the scene value
    // itself when nothing previews it)
    anim::ChannelRest rest;
    auto it = previewing ? session.rest.find(entity) : session.rest.end();
    if (it != session.rest.end() && it->second.hasTransform){
        rest = it->second;
    }else{
        takeRest(scene, entity, rest, track.channel, false);
    }
    if (!rest.hasTransform) return true;
    switch (track.channel){
        case ClipChannel::Position: {
            const Matrix4 matrix = scene->getSystem<ActionSystem>()->getRelativeTrackMatrix(entity, rest.position);
            const Vector3 offset = matrix.inverse() * Vector3(out[0], out[1], out[2]);
            out = {offset.x, offset.y, offset.z};
            break;
        }
        case ClipChannel::Rotation: {
            const Quaternion offset = rest.rotation.inverse() * quaternionFromXYZW(out.data());
            quaternionToXYZW(offset, out.data());
            break;
        }
        case ClipChannel::Scale: {
            const Vector3& base = rest.scale;
            out = {base.x != 0.0f ? out[0] / base.x : 1.0f, base.y != 0.0f ? out[1] / base.y : 1.0f, base.z != 0.0f ? out[2] / base.z : 1.0f};
            break;
        }
        default:
            break;
    }
    return true;
}

bool AnimationClipController::addProperty(const std::string& path, ClipChannel channel, size_t* newTrack, std::string* error){
    if (!editable(error)) return false;
    if (channel == ClipChannel::Property) return fail(error, "component properties can't be animated yet");
    std::vector<anim::PathSegment> segments;
    if (!anim::parsePath(path, segments)) return fail(error, "'" + path + "' is not a valid path");
    ClipData data = document->data();
    if (data.findTrack(path, channel) >= 0){
        return fail(error, "the clip already animates " + path + " : " + anim::channelName(channel));
    }
    ClipTrackData track;
    track.path = path;
    track.channel = channel;
    std::vector<float> value;
    if (!currentTrackValue(track, value)){
        SceneProject* sceneProject = nullptr;
        return fail(error, valueRoot(&sceneProject) == NULL_ENTITY
            ? "no GameObject is bound to read the current value from"
            : "no GameObject with that component at '" + path + "' under the bound root");
    }
    setKeyAt(track, time, value);
    data.tracks.push_back(track);
    if (!commitEdit(data, "Add Property", "", error)) return false;
    if (newTrack){
        const int index = document->data().findTrack(path, channel);
        *newTrack = index >= 0 ? (size_t)index : 0;
    }
    return true;
}

bool AnimationClipController::keyTracks(const std::vector<size_t>& tracks, float atTime, const std::string& mergeKey, std::string* error){
    if (!editable(error)) return false;
    if (!finite(atTime) || atTime < 0.0f) return fail(error, "the key time must be 0 or more");
    ClipData data = document->data();
    std::vector<size_t> indices = tracks;
    if (indices.empty()){
        for (size_t i = 0; i < data.tracks.size(); i++) indices.push_back(i);
    }
    size_t keyed = 0;
    for (size_t index : indices){
        if (index >= data.tracks.size()) return fail(error, "no track " + std::to_string(index));
        ClipTrackData& track = data.tracks[index];
        std::vector<float> value;
        if (!currentTrackValue(track, value)) continue;
        setKeyAt(track, atTime, value);
        keyed++;
    }
    if (keyed == 0) return fail(error, "no bound track to key (missing bindings are skipped)");
    return commitEdit(data, "Add Key", mergeKey, error);
}

bool AnimationClipController::keyAll(float atTime, std::string* error){
    return keyTracks({}, atTime, "", error);
}

bool AnimationClipController::keyModified(float atTime, std::string* error){
    if (!editable(error)) return false;
    const ClipData& data = document->data();
    std::shared_ptr<const AnimationClip> clip = document->clip();
    std::vector<anim::TrackSample> samples;
    ActionSystem::sampleClip(*clip, anim::wrapClipTime(atTime, clip->getLength(), ClipWrapMode::ClampForever), samples);
    std::vector<size_t> changed;
    for (size_t i = 0; i < data.tracks.size(); i++){
        const ClipTrackData& track = data.tracks[i];
        std::vector<float> value;
        if (!currentTrackValue(track, value)) continue;
        const int built = clip->findTrack(track.path, track.channel);
        if (built < 0 || (size_t)built >= samples.size() || !samples[(size_t)built].valid){
            changed.push_back(i);
            continue;
        }
        if (!sameValue(track.channel, value, sampleValue(samples[(size_t)built]))) changed.push_back(i);
    }
    if (changed.empty()) return fail(error, "no bound channel differs from the clip at the playhead");
    return keyTracks(changed, atTime, "", error);
}

bool AnimationClipController::setKeyValue(const KeyRef& key, const std::vector<float>& value, const std::string& mergeKey, std::string* error){
    if (!editable(error)) return false;
    ClipData data = document->data();
    if (key.track >= data.tracks.size() || key.key >= data.tracks[key.track].keys.size()) return fail(error, "no such key");
    ClipTrackData& track = data.tracks[key.track];
    for (float v : value){
        if (!finite(v)) return fail(error, "key values must be finite numbers");
    }
    size_t width = ClipData::valueWidth(track.channel, value.size(), track.points.size());
    if (track.channel == ClipChannel::Morph){
        if (value.empty() || value.size() > MAX_MORPHTARGETS){
            return fail(error, "a blend shape key holds 1 to " + std::to_string(MAX_MORPHTARGETS) + " weights");
        }
        width = value.size();
    }
    if (value.size() != width){
        return fail(error, "a " + std::string(anim::channelName(track.channel)) + " key holds " + std::to_string(width) + " numbers");
    }
    ClipKey& edited = track.keys[key.key];
    edited.value = value;
    if (edited.hasTangents() && edited.inTangent.size() != value.size()){
        edited.inTangent.assign(value.size(), 0.0f);
        edited.outTangent.assign(value.size(), 0.0f);
    }
    return commitEdit(data, "Edit Key", mergeKey, error);
}

namespace{
    // the dragged keys at their base times moved by `delta` (and their values by `valueDelta`)
    bool moveKeyData(ClipData& data, const std::vector<AnimationClipController::KeyRef>& keys, float delta,
                     const std::vector<float>* valueDelta, std::string* error){
        std::set<std::pair<size_t, size_t>> unique;
        for (const AnimationClipController::KeyRef& ref : keys){
            if (ref.track >= data.tracks.size() || ref.key >= data.tracks[ref.track].keys.size()) return fail(error, "no such key");
            if (!unique.insert({ref.track, ref.key}).second) continue;
            ClipKey& key = data.tracks[ref.track].keys[ref.key];
            key.time = std::max(0.0f, key.time + delta);
            if (valueDelta){
                for (size_t i = 0; i < key.value.size() && i < valueDelta->size(); i++) key.value[i] += (*valueDelta)[i];
            }
        }
        return true;
    }
}

bool AnimationClipController::moveKeys(const std::vector<KeyRef>& keys, float deltaTime, const std::string& mergeKey,
                                       std::vector<KeyRef>* moved, std::string* error){
    if (!editable(error)) return false;
    if (!finite(deltaTime)) return fail(error, "the time offset must be a finite number");
    ClipData data = document->data();
    std::vector<DragIdentity> identities;
    for (const KeyRef& ref : keys){
        if (ref.track >= data.tracks.size() || ref.key >= data.tracks[ref.track].keys.size()) return fail(error, "no such key");
        const ClipTrackData& track = data.tracks[ref.track];
        identities.push_back({track.path, track.channel, std::max(0.0f, track.keys[ref.key].time + deltaTime)});
    }
    if (!moveKeyData(data, keys, deltaTime, nullptr, error)) return false;
    if (!commitEdit(data, "Move Keys", mergeKey, error)) return false;
    if (moved){
        moved->clear();
        for (const DragIdentity& identity : identities){
            const int track = document->data().findTrack(identity.path, identity.channel);
            if (track < 0) continue;
            const int key = nearestKey(document->data().tracks[(size_t)track], identity.time, 1e-4f);
            if (key >= 0) moved->push_back({(size_t)track, (size_t)key});
        }
    }
    return true;
}

bool AnimationClipController::beginKeyDrag(const std::vector<KeyRef>& keys, std::string* error){
    if (!editable(error)) return false;
    if (keys.empty()) return fail(error, "no keys to drag");
    const ClipData& data = document->data();
    std::vector<DragIdentity> identities;
    for (const KeyRef& ref : keys){
        if (ref.track >= data.tracks.size() || ref.key >= data.tracks[ref.track].keys.size()) return fail(error, "no such key");
        const ClipTrackData& track = data.tracks[ref.track];
        identities.push_back({track.path, track.channel, track.keys[ref.key].time});
    }
    dragging = true;
    dragBase = data;
    dragKeys = keys;
    dragIdentities = identities;
    dragRevision = document->revision();
    dragMergeKey = "keydrag#" + std::to_string(++dragSerial);
    return true;
}

bool AnimationClipController::updateKeyDrag(float totalDeltaTime, const std::vector<float>* valueDelta,
                                            std::vector<KeyRef>* moved, std::string* error){
    if (!dragging) return fail(error, "no key drag in progress");
    if (!editable(error)){
        endKeyDrag();
        return false;
    }
    if (!finite(totalDeltaTime)) return fail(error, "the time offset must be a finite number");
    if (document->revision() != dragRevision){
        // undone, reloaded or edited elsewhere: the base is not the clip any more
        endKeyDrag();
        return fail(error, "the clip changed during the drag");
    }
    // the base moved by the total offset: keys passed over are back where they were
    ClipData data = dragBase;
    if (!moveKeyData(data, dragKeys, totalDeltaTime, valueDelta, error)) return false;
    if (!commitEdit(data, valueDelta ? "Drag Keys" : "Move Keys", dragMergeKey, error)) return false;
    dragRevision = document->revision();
    if (moved){
        moved->clear();
        for (const DragIdentity& identity : dragIdentities){
            const int track = document->data().findTrack(identity.path, identity.channel);
            if (track < 0) continue;
            const int key = nearestKey(document->data().tracks[(size_t)track], std::max(0.0f, identity.time + totalDeltaTime), 1e-4f);
            if (key >= 0) moved->push_back({(size_t)track, (size_t)key});
        }
    }
    return true;
}

void AnimationClipController::endKeyDrag(){
    dragging = false;
    dragKeys.clear();
    dragIdentities.clear();
    dragBase = ClipData();
    dragMergeKey.clear();
}

bool AnimationClipController::isDraggingKeys() const{
    return dragging;
}

bool AnimationClipController::findKey(const ClipData& data, const std::string& path, ClipChannel channel, float time, KeyRef& out){
    int track = data.findTrack(path, channel);
    if (track < 0) return false;
    const std::vector<ClipKey>& keys = data.tracks[(size_t)track].keys;
    for (size_t k = 0; k < keys.size(); k++){
        float difference = keys[k].time - time;
        if (difference <= anim::ANIM_TIME_EPSILON && difference >= -anim::ANIM_TIME_EPSILON){
            out.track = (size_t)track;
            out.key = k;
            return true;
        }
    }
    return false;
}

bool AnimationClipController::deleteKeys(const std::vector<KeyRef>& keys, std::string* error){
    if (!editable(error)) return false;
    ClipData data = document->data();
    std::map<size_t, std::set<size_t>> byTrack;
    for (const KeyRef& ref : keys){
        if (ref.track >= data.tracks.size() || ref.key >= data.tracks[ref.track].keys.size()) return fail(error, "no such key");
        byTrack[ref.track].insert(ref.key);
    }
    if (byTrack.empty()) return true;
    // from the back, so the indices before stay valid; a track without keys goes (Unity)
    for (auto track = byTrack.rbegin(); track != byTrack.rend(); ++track){
        std::vector<ClipKey>& trackKeys = data.tracks[track->first].keys;
        for (auto key = track->second.rbegin(); key != track->second.rend(); ++key) trackKeys.erase(trackKeys.begin() + (long)*key);
        if (trackKeys.empty()) data.tracks.erase(data.tracks.begin() + (long)track->first);
    }
    return commitEdit(data, "Delete Keys", "", error);
}

bool AnimationClipController::setEase(const std::vector<KeyRef>& keys, EaseType ease, std::string* error){
    if (!editable(error)) return false;
    if (ease == EaseType::CUSTOM) return fail(error, "a custom ease can't be stored in a clip");
    ClipData data = document->data();
    std::set<size_t> tracks;
    for (const KeyRef& ref : keys){
        if (ref.track >= data.tracks.size() || ref.key >= data.tracks[ref.track].keys.size()) return fail(error, "no such key");
        data.tracks[ref.track].keys[ref.key].ease = ease;
        tracks.insert(ref.track);
    }
    // tangents win over eases: a track given an ease drops its tangents (the curves become eased)
    for (size_t track : tracks){
        for (ClipKey& key : data.tracks[track].keys){
            key.inTangent.clear();
            key.outTangent.clear();
        }
    }
    return commitEdit(data, "Set Interpolation", "", error);
}

bool AnimationClipController::removeTrack(size_t track, std::string* error){
    if (!editable(error)) return false;
    ClipData data = document->data();
    if (track >= data.tracks.size()) return fail(error, "no track " + std::to_string(track));
    data.tracks.erase(data.tracks.begin() + (long)track);
    return commitEdit(data, "Remove Property", "", error);
}

bool AnimationClipController::retargetTrack(size_t track, const std::string& newPath, std::string* error){
    if (!editable(error)) return false;
    ClipData data = document->data();
    if (track >= data.tracks.size()) return fail(error, "no track " + std::to_string(track));
    std::vector<anim::PathSegment> segments;
    if (!anim::parsePath(newPath, segments)) return fail(error, "'" + newPath + "' is not a valid path");
    ClipTrackData& edited = data.tracks[track];
    if (edited.path == newPath) return true;
    const int other = data.findTrack(newPath, edited.channel);
    if (other >= 0 && (size_t)other != track){
        return fail(error, "the clip already animates " + newPath + " : " + anim::channelName(edited.channel));
    }
    edited.path = newPath;
    return commitEdit(data, "Retarget Property", "", error);
}

bool AnimationClipController::commit(const ClipData& data, const std::string& undoName, const std::string& mergeKey, std::string* error){
    return commitEdit(data, undoName, mergeKey, error);
}

// ---- record ----

void AnimationClipController::setRecording(bool on){
    if (on == recording) return;
    if (on){
        if (!editable(nullptr)) return;
        // Record keys what the preview shows
        if (!isPreviewing() && !startPreview(nullptr)) return;
        recording = true;
        ClipRecordListener listener;
        listener.claims = [this](uint32_t id, Entity entity, ClipChannel channel){ return recordClaims(id, entity, channel); };
        listener.record = [this](uint32_t id, Entity entity, const ClipRecordValue& value, const std::string& mergeKey){
            recordValue(id, entity, value, mergeKey);
        };
        recordListener = ClipRecordHooks::addRecordListener(listener);
        return;
    }
    recording = false;
    ClipRecordHooks::removeRecordListener(recordListener);
    recordListener = 0;
}

bool AnimationClipController::isRecording() const{
    return recording;
}

bool AnimationClipController::recordClaims(uint32_t id, Entity entity, ClipChannel channel) const{
    if (!recording || !isPreviewing() || id != previewSceneId || channel == ClipChannel::Property) return false;
    if (!document || document->isReadOnly() || !document->exists()) return false;
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    if (!sceneProject) return false;
    // anything under the root that has the channel's component: a new track when it has none
    std::string path;
    if (!anim::relativePath(sceneProject->scene, previewRoot, entity, path)) return false;
    std::vector<float> value;
    return anim::readChannelValue(sceneProject->scene, entity, channel, {}, value);
}

void AnimationClipController::recordValue(uint32_t id, Entity entity, const ClipRecordValue& value, const std::string& mergeKey){
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    if (!recordClaims(id, entity, value.channel) || !sceneProject) return;
    Scene* scene = sceneProject->scene;
    std::string path;
    if (!anim::relativePath(scene, previewRoot, entity, path)) return;

    // the rest is the scene's value before the first live write (stop puts it back)
    takeRest(scene, entity, session.rest[entity], value.channel, false);
    const std::pair<Entity, ClipChannel> entry(entity, value.channel);
    if (std::find(session.written.begin(), session.written.end(), entry) == session.written.end()) session.written.push_back(entry);
    anim::writeChannelValue(scene, entity, value.channel, value.points, value.value);

    ClipData data = document->data();
    int index = data.findTrack(path, value.channel);
    if (index < 0){
        ClipTrackData track;
        track.path = path;
        track.channel = value.channel;
        data.tracks.push_back(track);
        index = (int)data.tracks.size() - 1;
    }
    ClipTrackData& track = data.tracks[(size_t)index];
    std::vector<float> keyValue;
    if (value.channel == ClipChannel::Lattice){
        // the track's points and the edited ones, every key laid out on them
        std::vector<int> points = track.points;
        for (int point : value.points){
            if (std::find(points.begin(), points.end(), point) == points.end()) points.push_back(point);
        }
        std::sort(points.begin(), points.end());
        if (points != track.points){
            for (ClipKey& key : track.keys){
                std::vector<float> laid(points.size() * 3, 0.0f);
                for (size_t p = 0; p < track.points.size() && p * 3 + 2 < key.value.size(); p++){
                    const size_t at = (size_t)(std::find(points.begin(), points.end(), track.points[p]) - points.begin());
                    for (size_t c = 0; c < 3; c++) laid[at * 3 + c] = key.value[p * 3 + c];
                }
                key.value = laid;
                key.inTangent.clear();
                key.outTangent.clear();
            }
            track.points = points;
        }
        if (!anim::readChannelValue(scene, entity, ClipChannel::Lattice, track.points, keyValue)) return;
    }else if (!currentTrackValue(track, keyValue)){
        return;
    }
    setKeyAt(track, time, keyValue);
    std::string error;
    if (!commitEdit(data, "Record", mergeKey, &error)) Out::warning("Record: %s", error.c_str());
}

// ---- App integration ----

void AnimationClipController::stopTransientPreviews(){
    stopPreview();
}

bool AnimationClipController::ownsUndo(bool sceneViewFocused) const{
    return (lastActionWasClipEdit && document) || (recording && sceneViewFocused);
}

bool AnimationClipController::ownsUndo(const ClipUndoFocus& focus) const{
    if (focus.animationWindow && lastActionWasClipEdit && document) return true;
    return recording && (focus.sceneView || focus.inspector);
}

void AnimationClipController::setLastActionWasClipEdit(bool clipEdit){
    lastActionWasClipEdit = clipEdit;
}

void AnimationClipController::setTakesKeys(bool takes){
    windowKeys = takes;
}

bool AnimationClipController::takesKeys() const{
    return windowKeys;
}

void AnimationClipController::sceneClosed(uint32_t id){
    if (id == 0) return;
    // the scene and its entities go: nothing to restore
    if (previewing && previewSceneId == id) endPreview(false);
    if (sceneId == id){
        if (!stateName.empty()) closeClip();
        sceneId = 0;
        root = NULL_ENTITY;
        stateName.clear();
    }
}
