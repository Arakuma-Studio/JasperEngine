// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationClipController.h"
#include "util/FileUtils.h"

#include "animation/AnimatorAssetDocument.h"
#include "animation/AvatarMaskWeights.h"

#include "Out.h"
#include "Project.h"
#include "Scene.h"
#include "animation/AnimationClipAppHooks.h"
#include "animation/AnimationClipIO.h"
#include "animation/ClipBinding.h"
#include "animation/ClipConverters.h"
#include "animation/ClipEditorUtil.h"
#include "animation/KeyframeSampling.h"
#include "animation/ModelSubClips.h"
#include "animation/PropertyBindings.h"
#include "component/AnimationPlayerComponent.h"
#include "component/AudioSourceComponent.h"
#include "component/LatticeComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/ScriptComponent.h"
#include "component/Transform.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "lattice/LatticeSolver.h"
#include "math/AABB.h"
#include "math/Matrix4.h"
#include "pool/AnimationClipPool.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "util/HybridArray.h"

#include <algorithm>
#include <chrono>
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
            // by the one history timeline, with or without a controller: the Inspector's clip
            // commits count too (R5c)
            hooks.ownsUndo = [](){
                return AnimationClipController::newestUndoIsClipCommit(nullptr);
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
            // every frame, Animation window open or not: previews advance, deferred stops end
            hooks.frame = [](Project* project, double dt, int frame){
                auto it = controllers().find(project);
                if (it != controllers().end()) it->second->update(dt, frame);
            };
            hooks.projectClosed = [](){
                std::vector<Project*> projects;
                for (auto& entry : controllers()) projects.push_back(entry.first);
                for (Project* project : projects) AnimationClipController::release(project);
            };
        }
    };
    const AppHooksInstaller appHooksInstaller;

    // R5b: when each history's undoable steps were taken, to find the newest across the scene
    // and project histories. Kept from CommandHistory::onSceneModified (every add, merge, insert,
    // undo and redo) and beforeStep (an undo or redo follows), chained after the App's own
    // listeners by App::engineInit (installHistoryTimeline) or, without the App (tests, CLI),
    // when the first controller is made or the first clip commit is marked.
    struct HistoryTimeline{
        struct Steps{
            std::vector<uint64_t> stamps;   // per position: when that step was done
            std::vector<bool> clip;         // per position: a clip commit of a controller
            std::vector<uint64_t> undone;   // per position: when that step was undone (0: not undone)
            uint64_t lastNew = 0;           // when its last new step (not a redo) was made
            size_t last = 0;
        };
        bool installed = false;
        uint64_t clock = 0;
        std::map<size_t, Steps> histories;
        std::set<size_t> stepping;
        Project* project = nullptr;         // whose history id 0 is
    };

    HistoryTimeline& timeline(){
        static HistoryTimeline* instance = new HistoryTimeline();
        return *instance;
    }

    CommandHistory* historyOf(size_t id){
        HistoryTimeline& t = timeline();
        if (id == NULL_PROJECT_SCENE) return t.project ? t.project->getProjectCommandHistory() : nullptr;
        return CommandHandle::get(id);
    }

    void historyChanged(size_t id){
        HistoryTimeline& t = timeline();
        CommandHistory* history = historyOf(id);
        if (!history) return;
        HistoryTimeline::Steps& steps = t.histories[id];
        const size_t position = history->position();
        const bool step = t.stepping.erase(id) > 0;
        if (!step){
            // a new step (or one merged into the last): the redo tail is gone
            steps.stamps.resize(position, 0);
            steps.clip.resize(position, false);
            steps.undone.resize(position, 0);
            if (position > 0){
                steps.stamps[position - 1] = ++t.clock;
                steps.clip[position - 1] = false;
                steps.undone[position - 1] = 0;
                steps.lastNew = t.clock;
            }
        }else if (position > steps.last){
            // a redo: the step is done again now (and stays what it was)
            if (steps.stamps.size() < position){
                steps.stamps.resize(position, 0);
                steps.clip.resize(position, false);
                steps.undone.resize(position, 0);
            }
            steps.stamps[position - 1] = ++t.clock;
            steps.undone[position - 1] = 0;
        }else if (position < steps.last){
            // an undo: the step at `position` is the next redo, undone now
            if (steps.stamps.size() <= position){
                steps.stamps.resize(position + 1, 0);
                steps.clip.resize(position + 1, false);
                steps.undone.resize(position + 1, 0);
            }
            steps.undone[position] = ++t.clock;
        }
        steps.last = position;
    }

    // after a scene undo or redo: the Animation Player entries it gave back (an old path hint
    // of a clip renamed or moved since) point at their clips by GUID again (9.R8), so the
    // Inspector, the window and the edit preview find them without a reopen
    void resolveClipRefsAfterStep(size_t id){
        Project* project = timeline().project;
        if (!project || id == NULL_PROJECT_SCENE) return;
        SceneProject* sceneProject = project->getScene((uint32_t)id);
        if (!sceneProject || !sceneProject->scene || sceneProject->playState != ScenePlayState::STOPPED) return;
        if (project->resolveRegistryClipRefs(sceneProject->scene, true)) sceneProject->isModified = true;
    }

    void chainTimeline(){
        std::function<void(size_t, bool)> modified = CommandHistory::onSceneModified;
        CommandHistory::onSceneModified = [modified](size_t id, bool structural){
            // an undo / redo step (beforeStep marked it) has run: CommandHistory reports it here
            // right after the step, as it does for a new command
            const bool step = timeline().stepping.count(id) > 0;
            if (step) resolveClipRefsAfterStep(id);
            if (modified) modified(id, structural);
            historyChanged(id);
        };
        std::function<void(size_t)> before = CommandHistory::beforeStep;
        CommandHistory::beforeStep = [before](size_t id){
            if (before) before(id);
            timeline().stepping.insert(id);
        };
    }

    void installTimeline(Project* project){
        HistoryTimeline& t = timeline();
        if (project) t.project = project;
        if (t.installed) return;
        t.installed = true;
        chainTimeline();
    }

    // the next redoable step of a history: when it was undone (0: none, or before the timeline)
    // and whether it is a clip commit
    uint64_t redoStep(size_t id, bool* clip){
        if (clip) *clip = false;
        CommandHistory* history = historyOf(id);
        if (!history || !history->canRedo()) return 0;
        HistoryTimeline& t = timeline();
        auto it = t.histories.find(id);
        const size_t position = history->position();
        if (it == t.histories.end() || position >= it->second.undone.size()) return 0;
        if (clip) *clip = it->second.clip[position];
        return it->second.undone[position];
    }

    // the top undoable step of a history: when it was done (0: before the timeline) and whether
    // it is a clip commit
    uint64_t topStep(size_t id, bool* clip){
        if (clip) *clip = false;
        CommandHistory* history = historyOf(id);
        if (!history) return 0;
        HistoryTimeline& t = timeline();
        auto it = t.histories.find(id);
        const size_t position = history->position();
        if (it == t.histories.end() || position == 0 || position > it->second.stamps.size()) return 0;
        if (clip) *clip = it->second.clip[position - 1];
        return it->second.stamps[position - 1];
    }

    bool finite(float value){
        return std::isfinite(value);
    }

    // `entity` is a part of a model under `root` in the legacy layout: its path changes when an
    // Animation Player reloads the model in its node hierarchy (R3), so a key made on it now would
    // stop binding then (Record and Add Property refuse it and say why)
    bool legacyModelPart(Scene* scene, Entity root, Entity entity){
        std::string now;
        std::string later;
        if (!anim::relativePath(scene, root, entity, now)) return false;
        return ClipConverters::clipPath(scene, root, entity, later) && later != now;
    }

    // (the model reloads in its node hierarchy once an Animation Player on it or above it plays it:
    // adding the player does it, and a player that has one already offers "Reload Models in Node
    // Hierarchy" in its Inspector Bindings and the Animation window)
    const char* const LEGACY_PART_REASON = "is part of a model in the legacy layout, whose paths change when an Animation Player "
                                           "reloads it in its node hierarchy: add the player (or its Reload Models in Node "
                                           "Hierarchy) first, or these keys would stop binding then";
    const char* const LEGACY_ROOT_REASON = "carries its model file's root transform in the legacy layout, which moves to the root "
                                           "node when an Animation Player reloads the model in its node hierarchy: add the "
                                           "player (or its Reload Models in Node Hierarchy) first, or these keys would apply that "
                                           "transform twice then";

    // Why Record and Add Property leave `channel` of `entity` (under or at `root`) alone, null when
    // they don't: a legacy model part (its path changes with the node hierarchy), or a channel of a
    // legacy model entity standing in for a glTF scene root that is not the identity (the reload
    // moves that matrix to the root node: Convert takes it off its keys, a key made here could
    // not). The root's rotation and scale change the Rotation and Scale keys, its translation
    // every Transform key; an identity root changes none.
    const char* legacyRefusal(Scene* scene, Entity root, Entity entity, ClipChannel channel){
        if (legacyModelPart(scene, root, entity)) return LEGACY_PART_REASON;
        if (channel != ClipChannel::Position && channel != ClipChannel::Rotation && channel != ClipChannel::Scale) return nullptr;
        const ModelComponent* model = scene->findComponent<ModelComponent>(entity);
        std::shared_ptr<MeshSystem> meshes = scene->getSystem<MeshSystem>();
        Matrix4 sceneRoot;
        if (!model || !meshes || !MeshSystem::isLegacyLayout(*model) || !meshes->getLegacyRootTransform(entity, sceneRoot)) return nullptr;
        Vector3 position;
        Vector3 scale;
        Quaternion rotation;
        sceneRoot.decompose(position, scale, rotation);
        const float eps = 1e-5f;
        const bool translated = position.length() > eps;
        const bool turnedOrScaled = std::fabs(std::fabs(rotation.w) - 1.0f) > 1e-6f ||
                                    std::fabs(scale.x - 1.0f) > eps || std::fabs(scale.y - 1.0f) > eps || std::fabs(scale.z - 1.0f) > eps;
        if (translated || (turnedOrScaled && channel != ClipChannel::Position)) return LEGACY_ROOT_REASON;
        return nullptr;
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
    void setKeyAt(ClipTrackData& track, float time, const std::vector<float>& given){
        // a rotation on the side of the key it replaces (or follows): no Hermite flip
        const std::vector<float> value = clipui::alignedRotationKeyValue(track, time, given);
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
            case ClipChannel::HumanoidPosition:
                return {sample.vector.x, sample.vector.y, sample.vector.z};
            case ClipChannel::Rotation:
            case ClipChannel::Humanoid: {
                std::vector<float> out(4);
                quaternionToXYZW(sample.rotation, out.data());
                return out;
            }
            case ClipChannel::Morph:
            case ClipChannel::Property:
                return sample.weights;
            case ClipChannel::Lattice: {
                std::vector<float> out;
                for (const Vector3& offset : sample.offsets) out.insert(out.end(), {offset.x, offset.y, offset.z});
                return out;
            }
        }
        return {};
    }

    bool sameValue(ClipChannel channel, const std::vector<float>& a, const std::vector<float>& b){
        const float tolerance = 1e-4f;
        if (isRotationChannel(channel) && a.size() == 4 && b.size() == 4){
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
            case ClipChannel::Humanoid:
            case ClipChannel::HumanoidPosition:
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
            case ClipChannel::Humanoid:
            case ClipChannel::HumanoidPosition:
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
            case ClipChannel::Humanoid:
            case ClipChannel::HumanoidPosition:
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

    // ---- property tracks (anim-properties spec 8, 10) ----

    // how a property track binds on `entity`
    anim::PropertyBinding propertyBindingOf(Scene* scene, Entity entity, const std::string& component, const std::string& property, size_t keyWidth){
        anim::PropertyRef ref;
        anim::parsePropertyRef(component, property, ref);
        return anim::bindProperty(scene, entity, ref, keyWidth);
    }

    // a property track's key value now: its property read in key space (a swizzle track its
    // component)
    bool currentPropertyValue(Scene* scene, Entity entity, const ClipTrackData& track, std::vector<float>& out){
        out.clear();
        const anim::PropertyBinding binding = propertyBindingOf(scene, entity, track.component, track.property, track.width);
        if (binding.status != anim::PropertyBindStatus::Bound) return false;
        float value[4] = {0, 0, 0, 0};
        if (!anim::readPropertyKeySpace(scene, entity, binding, value)) return false;
        if (binding.swizzle >= 0) out = {value[binding.swizzle]};
        else out.assign(value, value + binding.width);
        // stored values come back with float noise: whole numbers stay whole (ints, bools, frames)
        if (binding.discrete || binding.interpolateInt){
            for (float& v : out) v = std::round(v);
        }
        return true;
    }

    // the rest of a previewed property (taken the first time; its binding every time)
    anim::PropertyRest* takePropertyRest(Scene* scene, Entity entity, anim::ChannelRest& rest, const anim::PropertyBinding& binding){
        if (binding.status != anim::PropertyBindStatus::Bound) return nullptr;
        anim::PropertyRest& property = rest.properties[binding.key];
        if (!property.has && !anim::readPropertyRest(scene, entity, binding, property)) return nullptr;
        property.binding = binding;
        // the binding of the whole property (a swizzle track's component is written as part of it)
        property.binding.swizzle = -1;
        return &property;
    }

    // the identity of a key (property tracks: their component and property)
    template<typename Identity>
    Identity identityOf(const ClipTrackData& track, float time){
        Identity identity;
        // (a humanoid track's identity path is its bone name: ClipData::findTrack)
        identity.path = clipui::trackIdentityPath(track);
        identity.channel = track.channel;
        identity.time = time;
        identity.component = track.component;
        identity.property = track.property;
        return identity;
    }

    template<typename Identity>
    int trackOf(const ClipData& data, const Identity& identity){
        if (identity.channel == ClipChannel::Property) return data.findPropertyTrack(identity.path, identity.component, identity.property);
        return data.findTrack(identity.path, identity.channel);
    }

    std::string readOnlyError(const AnimationClipDocument& document){
        return "'" + document.path() + "' is a model's animation (read-only): Extract to .anim to edit it";
    }
}

// ---- construction ----

AnimationClipController::AnimationClipController(Project* project): project(project){
    installTimeline(project);
    // scene commands, undo and redo writing a previewed channel: the command sees (and edits)
    // the rest, which becomes the snapshot (9.R6)
    beforeWriteListener = ClipRecordHooks::addBeforeChannelWrite([this](uint32_t id, Entity entity, ClipChannel channel){
        beforeSceneWrite(id, entity, channel);
    });
    afterWriteListener = ClipRecordHooks::addAfterChannelWrite([this](uint32_t id, Entity entity, ClipChannel channel){
        afterSceneWrite(id, entity, channel);
    });
    previewQuery = ClipRecordHooks::addPreviewQuery([this](uint32_t id, Entity entity, ClipChannel channel){
        const std::pair<Entity, ClipChannel> entry(entity, channel);
        return isPreviewing() && id == previewSceneId &&
               std::find(session.written.begin(), session.written.end(), entry) != session.written.end();
    });
    // the same for previewed properties (anim-properties: an Inspector field, set_properties)
    beforePropertyListener = ClipRecordHooks::addBeforePropertyWrite([this](uint32_t id, Entity entity, const ClipPropertyKey& property){
        beforePropertySceneWrite(id, entity, property);
    });
    afterPropertyListener = ClipRecordHooks::addAfterPropertyWrite([this](uint32_t id, Entity entity, const ClipPropertyKey& property){
        afterPropertySceneWrite(id, entity, property);
    });
}

AnimationClipController::~AnimationClipController(){
    // never touches scenes (release() restored them while they existed)
    ClipRecordHooks::removeChannelWriteListener(beforeWriteListener);
    ClipRecordHooks::removeChannelWriteListener(afterWriteListener);
    ClipRecordHooks::removeChannelWriteListener(previewQuery);
    ClipRecordHooks::removeChannelWriteListener(beforePropertyListener);
    ClipRecordHooks::removeChannelWriteListener(afterPropertyListener);
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
    previewOver.clear();
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
    const fs::path documentFile = document->absolutePath().lexically_normal();
    for (size_t i = 0; i < entries.size() && i < names.size(); i++){
        const std::string& clip = entries[i].clip;
        if (clip.empty()) continue;
        anim::SubClipReference reference;
        if (anim::parseSubClipReference(clip, reference)){
            // a sub-clip document's path is "<absolute model>#<clip name>"; an entry naming the
            // animation by its index only matches through the (cached) document
            const fs::path model = project->resolveAssetPath(editor::FileUtils::pathFromUtf8(reference.model)).lexically_normal();
            if (fs::path(model.generic_string() + "#" + reference.clip) == documentFile) return names[i];
            const std::string documentText = documentFile.generic_string();
            if (documentText.rfind(model.generic_string() + "#", 0) == 0 && !reference.clip.empty() &&
                    std::all_of(reference.clip.begin(), reference.clip.end(), [](char c){ return c >= '0' && c <= '9'; }) &&
                    AnimationClipDocument::open(project, clip, nullptr) == document){
                return names[i];
            }
            continue;
        }
        if (project->resolveAssetPath(editor::FileUtils::pathFromUtf8(clip)).lexically_normal() == documentFile) return names[i];
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
    pending.clear();
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
    // the document follows a rename or move of its file (AnimationClipDocument::filesMoved)
    return document ? document->path() : clipPath;
}

const std::string& AnimationClipController::getStateName() const{
    return stateName;
}

// ---- layers ----

int AnimationClipController::getStateLayer() const{
    if (!hasPlayer() || stateName.empty()) return 0;
    const AnimationPlayerComponent& player = sceneProjectOf(sceneId)->scene->getComponent<AnimationPlayerComponent>(root);
    const std::vector<std::string> names = anim::playerStateNames(player.clips);
    const auto it = std::find(names.begin(), names.end(), stateName);
    if (it == names.end()) return 0;
    const int layer = player.clips[(size_t)(it - names.begin())].layer;
    return layer >= 0 && (size_t)layer < anim::playerLayers(player).size() ? layer : 0;
}

std::string AnimationClipController::getLayerName(int layer) const{
    if (!hasPlayer()) return layer == 0 ? std::string(anim::BASE_LAYER_NAME) : std::string();
    const std::vector<AnimationPlayerLayer> layers = anim::playerLayers(sceneProjectOf(sceneId)->scene->getComponent<AnimationPlayerComponent>(root));
    return layer >= 0 && (size_t)layer < layers.size() ? layers[(size_t)layer].name : std::string();
}

std::vector<std::pair<std::string, std::vector<std::string>>> AnimationClipController::getStatesByLayer() const{
    std::vector<std::pair<std::string, std::vector<std::string>>> groups;
    if (!hasPlayer()) return groups;
    const AnimationPlayerComponent& player = sceneProjectOf(sceneId)->scene->getComponent<AnimationPlayerComponent>(root);
    const std::vector<AnimationPlayerLayer> layers = anim::playerLayers(player);
    const std::vector<std::string> names = anim::playerStateNames(player.clips);
    for (const AnimationPlayerLayer& layer : layers) groups.push_back({layer.name, {}});
    for (size_t i = 0; i < names.size(); i++){
        const int layer = player.clips[i].layer;
        groups[layer >= 0 && (size_t)layer < groups.size() ? (size_t)layer : 0].second.push_back(names[i]);
    }
    return groups;
}

void AnimationClipController::setPreviewOver(const std::string& baseState){
    if (previewOver == baseState) return;
    previewOver = baseState;
    if (previewing) apply();
}

const std::string& AnimationClipController::getPreviewOver() const{
    return previewOver;
}

std::vector<std::string> AnimationClipController::previewOverChoices() const{
    const std::vector<std::pair<std::string, std::vector<std::string>>> groups = getStatesByLayer();
    return groups.empty() ? std::vector<std::string>() : groups[0].second;
}

std::vector<std::string> AnimationClipController::maskedTracks() const{
    std::vector<std::string> masked;
    if (!document || !hasPlayer()) return masked;
    const size_t trackCount = document->data().tracks.size();
    const int layer = getStateLayer();
    SceneProject* sceneProject = sceneProjectOf(sceneId);
    Scene* scene = sceneProject->scene;
    const AnimationPlayerComponent& player = scene->getComponent<AnimationPlayerComponent>(root);
    const std::vector<AnimationPlayerLayer> layers = anim::playerLayers(player);
    if (layer < 0 || (size_t)layer >= layers.size() || layers[(size_t)layer].mask.empty()) return masked;
    std::string error;
    std::shared_ptr<AnimatorAssetDocument> maskDocument = AnimatorAssetDocument::open(project, layers[(size_t)layer].mask, &error);
    if (!maskDocument || !maskDocument->loadError().empty()) return masked;
    // computed again when the clip, the mask, the hierarchy or the state change (at most twice a second)
    const std::string key = std::to_string(document->revision()) + ":" + std::to_string(maskDocument->revision()) + ":" +
                            std::to_string(scene->hierarchyRevision()) + ":" + std::to_string(sceneId) + ":" + std::to_string(root) + ":" +
                            stateName + ":" + layers[(size_t)layer].mask;
    const double now = (double)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0;
    if (key == maskedKey && maskedCache.size() == trackCount && now - maskedAt < 0.5) return maskedCache;
    maskedKey = key;
    maskedAt = now;
    const anim::AvatarMaskDef& mask = maskDocument->mask();
    anim::HumanoidSlots slots;
    if (mask.usesHumanoid()) slots = anim::resolveHumanoidSlots(scene, root);
    const std::vector<Entity> entries = anim::bindMaskPaths(mask, scene, root);
    const std::string reason = "masked out by " + fs::u8path(layers[(size_t)layer].mask).filename().string() + " on layer " + layers[(size_t)layer].name;
    masked.assign(trackCount, std::string());
    for (size_t t = 0; t < trackCount; t++){
        // humanoid tracks: their slot's bone on the root's rig
        const ClipTrackData& clipTrack = document->data().tracks[t];
        Entity target = NULL_ENTITY;
        if (isHumanoidChannel(clipTrack.channel)){
            std::shared_ptr<const anim::HumanoidSlots> humanoid = boundHumanoid();
            if (humanoid && clipTrack.channel == ClipChannel::Humanoid && clipTrack.humanBone < pose::HUMAN_BONE_COUNT){
                target = humanoid->target->slotEntities[(size_t)clipTrack.humanBone];
            }
            if (target == NULL_ENTITY) continue;
        }
        const std::string& path = clipTrack.path;
        if (target == NULL_ENTITY) target = path.empty() ? root : anim::resolvePath(scene, root, path);
        if (target == NULL_ENTITY) continue;
        if (anim::maskWeightOf(mask, scene, root, target, entries, slots.valid ? &slots : nullptr).weight <= 0.0f) masked[t] = reason;
    }
    maskedCache = masked;
    return masked;
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
    const fs::path target = project->resolveAssetPath(editor::FileUtils::pathFromUtf8(model)).lexically_normal();
    auto matches = [&](Entity entity){
        if (!scene->isEntityCreated(entity)) return false;
        if (const ModelComponent* component = scene->findComponent<ModelComponent>(entity)){
            if (!component->filename.empty() &&
                project->resolveAssetPath(FileUtils::pathFromUtf8(component->filename.path)).lexically_normal() == target) return true;
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
                ? "nothing to preview on: select a GameObject, or set the clip's Preview Model (preview.model)"
                : "nothing to preview on: no instance of '" + model + "' (the clip's preview model) in the open scene");
        }
    }
    if (sceneProject->playState != ScenePlayState::STOPPED){
        return fail(error, "the scene is playing or being saved: previews run in edit mode");
    }
    if (project->getSelectedSceneId() != targetScene){
        // a preview shows in the scene on screen and ends when another one is selected (R6)
        return fail(error, "the scene is not the selected one: select its tab to preview in it");
    }

    // one edit-mode pose owner per root: the Pose window, the Inspector's action preview or
    // another preview of this hierarchy restore their snapshots now (9.R7)
    ownerToken = PreviewOwner::acquire(targetScene, target, sceneProject->scene, PREVIEW_OWNER_NAME, [this](){ stopPreview(); });
    // what it writes: the entities it took a rest of (a first model load under the root is not one)
    PreviewOwner::setWrites(ownerToken, [this](Entity entity){ return previewing && session.rest.count(entity) > 0; });

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
    particles.stop(sceneProject ? sceneProject->scene : nullptr);
    particleTime = -1.0f;
    audioPreview.stopExit();
    session.reset();
    snapshot.reset();
    previewing = false;
    playing = false;
    stopPending = false;
    needsApply = false;
    unresolved.clear();
    pending.clear();
    shownValues.clear();
    previewSceneId = 0;
    previewRoot = NULL_ENTITY;
    // Record keys into the preview: it ends with it (Unity); one ended by a save or Play says so
    if (recording){
        setRecording(false);
        if (transientStop) recordStoppedNotice = true;
    }
    transientStop = false;
    ClipRecordHooks::endAllGestures();
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
    audioPreview.stopExit();
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
    audioPreview.stopExit();
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

void AnimationClipController::previewCues(float from, float delta, double dt){
    if (!document || delta == 0.0f) return;
    std::shared_ptr<const AnimationClip> built = document->clip();
    if (!built || built->getAudioCues().empty()) return;
    std::vector<anim::CrossedEvent> crossed;
    // the playback loops (update below): a crossing of the end is the loop's
    anim::collectCrossedCues(*built, ClipWrapMode::Loop, from, from + delta, false, crossed);
    const float length = getLength();
    const bool wraps = length > 0.0f && (from + delta > length || from + delta < 0.0f);
    if (wraps) audioPreview.stopExit();
    if (crossed.empty() || !TimelineAudioPreview::blocker().empty()) return;
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    for (const anim::CrossedEvent& cross : crossed){
        const ClipAudioCue& cue = built->getAudioCues()[cross.index];
        const uint32_t delay = TimelineAudioPreview::delayFrames(cross.position - from, delta, dt);
        const float pitch = cue.followSpeed ? std::fabs(playbackSpeed) : 1.0f;
        const bool exitStop = cue.stop == ClipCueStop::StateExit;
        if (cue.kind == ClipCueKind::Sound){
            audioPreview.play(project, cue.sound.path, cue.volumeDb, (float)(pitch * std::pow(2.0, cue.pitchSemitones / 12.0)), delay, exitStop);
        }else if (cue.kind == ClipCueKind::Source && sceneProject){
            // the source's own clip or event at its volume and speed (no 3D: preview voices are 2D)
            const Entity bound = anim::resolvePath(sceneProject->scene, previewRoot, cue.path);
            const AudioSourceComponent* audio = bound != NULL_ENTITY ? sceneProject->scene->findComponent<AudioSourceComponent>(bound) : nullptr;
            if (!audio) continue;
            const std::string sound = audio->event.empty() ? audio->filename.path : audio->event.path;
            const float volumeDb = audio->volume > 0.0 ? (float)(20.0 * std::log10(audio->volume)) : -80.0f;
            audioPreview.play(project, sound, volumeDb, pitch * audio->speed, delay, exitStop);
        }
        // a snapshot cue previews nothing: preview voices are outside the mixer's groups
    }
}

int AnimationClipController::previewAudioVoices(){
    return audioPreview.voiceCount();
}

uint64_t AnimationClipController::previewAudioStarts() const{
    return audioPreview.startedCount();
}

void AnimationClipController::update(double dt, int frame){
    if (frame == lastUpdateFrame) return;
    lastUpdateFrame = frame;
    update(dt);
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
    particles.pump(sceneProject->scene);
    // play mode, another scene shown, the previewed root deleted: the preview ends (restored)
    if ((sceneProject->playState != ScenePlayState::STOPPED && sceneProject->playState != ScenePlayState::SAVING) ||
        (project && project->getSelectedSceneId() != previewSceneId) || !sceneProject->scene->isEntityCreated(previewRoot)){
        stopPreview();
        return;
    }
    if (playing && finite((float)dt)){
        // the window's playback loops the clip (Unity), whatever its wrap mode
        const float length = getLength();
        const float from = time;
        previewCues(from, (float)dt * playbackSpeed, dt);
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
    // another scene tab shows: this one goes back as it was (also without an update)
    if (project && project->getSelectedSceneId() != previewSceneId){
        stopPreview();
        return false;
    }
    // destroyed entities leave the session (a recycled id must not get their rest)
    for (auto it = session.rest.begin(); it != session.rest.end();){
        if (!scene->isEntityCreated(it->first)) it = session.rest.erase(it);
        else ++it;
    }
    session.written.erase(std::remove_if(session.written.begin(), session.written.end(),
                                         [scene](const std::pair<Entity, ClipChannel>& entry){ return !scene->isEntityCreated(entry.first); }),
                          session.written.end());

    ActionSystem::PlayerPreview preview;
    // Preview over (anim layers spec 7.5, 9): the chosen base state at the playhead, the edited
    // clip on its layer at weight 1 with that layer's mask
    const int editedLayer = getStateLayer();
    if (!previewOver.empty() && editedLayer > 0 && hasPlayer() && previewRoot == root){
        const AnimationPlayerComponent& player = scene->getComponent<AnimationPlayerComponent>(root);
        preview.layered = true;
        for (const AnimationPlayerLayer& layer : anim::playerLayers(player)){
            ActionSystem::PlayerPreviewLayer previewLayer;
            previewLayer.weight = 1.0f;
            previewLayer.blend = layer.blend;
            previewLayer.mask = layer.mask;
            preview.layers.push_back(previewLayer);
        }
        ActionSystem::PlayerPreviewState base;
        base.state = previewOver;
        base.time = time;
        base.layer = 0;
        preview.states.push_back(base);
    }
    ActionSystem::PlayerPreviewState state;
    state.clip = document->clip();
    state.time = time;
    // the playhead is a clip time: past the end shows the end (playback loops in update)
    state.wrap = ClipWrapMode::ClampForever;
    state.layer = preview.layered ? editedLayer : 0;
    preview.states.push_back(state);
    ActionSystem* actions = scene->getSystem<ActionSystem>().get();
    ActionSystem::PlayerPreviewResult result = actions->previewPlayer(previewRoot, preview, session);

    // channels the clip no longer animates (a track removed or retargeted) show their rest
    for (const std::pair<Entity, ClipChannel>& entry : session.written){
        if (std::find(result.channels.begin(), result.channels.end(), entry) == result.channels.end()){
            writeRest(entry.first, entry.second);
        }
    }
    for (const std::pair<Entity, uint32_t>& entry : session.writtenProperties){
        if (std::find(result.properties.begin(), result.properties.end(), entry) == result.properties.end()){
            writePropertyRest(entry.first, entry.second);
        }
    }
    unresolved = result.unresolved;
    // the effects under the root at the playhead: played on while the preview plays forward,
    // simulated to it after a scrub or a loop
    particles.sync(scene, previewRoot, time, playing && particleTime >= 0.0f && time >= particleTime);
    particleTime = time;
    // the unkeyed values stay over the clip while the playhead stays
    if (!pending.empty()){
        if (pendingTime == time) writePending();
        else pending.clear();
    }
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

void AnimationClipController::writePropertyRest(Entity entity, uint32_t key){
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    if (!sceneProject || !sceneProject->scene->isEntityCreated(entity)) return;
    auto it = session.rest.find(entity);
    if (it == session.rest.end()) return;
    auto property = it->second.properties.find(key);
    if (property == it->second.properties.end() || !property->second.has) return;
    anim::restorePropertyRest(sceneProject->scene, entity, property->second);
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

std::vector<AnimationClipController::BoundProperty> AnimationClipController::sampledBoundProperties(size_t max) const{
    std::vector<BoundProperty> out;
    if (!document) return out;
    SceneProject* sceneProject = nullptr;
    const Entity target = valueRoot(&sceneProject);
    if (target == NULL_ENTITY || !sceneProject) return out;
    Scene* scene = sceneProject->scene;
    for (const ClipTrackData& track : document->data().tracks){
        if (out.size() >= max) break;
        if (track.channel != ClipChannel::Property || !track.known) continue;
        const Entity entity = anim::resolvePath(scene, target, track.path);
        BoundProperty bound;
        if (entity == NULL_ENTITY || !currentPropertyValue(scene, entity, track, bound.value)) continue;
        bound.path = track.path;
        bound.entity = entity;
        bound.component = track.component;
        bound.property = track.property;
        bound.kind = track.kind;
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
    if (!isPreviewing() || id != previewSceneId) return;
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    if (!sceneProject || sceneProject->playState != ScenePlayState::STOPPED) return;
    // the command reads and edits the rest, not the previewed value
    const std::pair<Entity, ClipChannel> entry(entity, channel);
    if (std::find(session.written.begin(), session.written.end(), entry) != session.written.end()){
        std::vector<float> shown;
        if (anim::readChannelValue(sceneProject->scene, entity, channel, {}, shown)){
            shownValues.erase(std::remove_if(shownValues.begin(), shownValues.end(),
                                             [&entry](const auto& value){ return value.first == entry; }), shownValues.end());
            shownValues.push_back({entry, shown});
        }
        writeRest(entity, channel);
    }
}

void AnimationClipController::afterSceneWrite(uint32_t id, Entity entity, ClipChannel channel){
    if (!isPreviewing() || id != previewSceneId) return;
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    if (!sceneProject || sceneProject->playState != ScenePlayState::STOPPED) return;
    // Record on: an edit it left alone (a scene step now) says why, when the edit arrives (not
    // for an undo or redo writing it)
    // (only an edit Record would take but for the legacy layout: the channels, documents and
    // entities recordClaims refuses for other reasons get no note)
    if (!timeline().stepping.count(id) && sceneProject->scene->isEntityCreated(entity) && recordEligible(id, entity, channel)){
        if (const char* refusal = legacyRefusal(sceneProject->scene, previewRoot, entity, channel)){
            ClipRecordHooks::note("Record leaves '" + sceneProject->scene->getEntityName(entity) + "' alone: it " + refusal);
        }
    }
    const std::pair<Entity, ClipChannel> entry(entity, channel);
    std::vector<float> shown;
    bool hadShown = false;
    for (auto value = shownValues.begin(); value != shownValues.end(); ++value){
        if (value->first == entry){
            shown = value->second;
            hadShown = true;
            shownValues.erase(value);
            break;
        }
    }
    auto it = session.rest.find(entity);
    if (it == session.rest.end() || !hasRestFor(it->second, channel)) return;
    needsApply = true;
    // the previewed value written back as it was shown is no edit: the rest stays (9.R6). Not
    // for components a command named explicitly (an Inspector field, set_properties): a value
    // set on purpose is the new rest even when the preview shows the same
    const ClipRecordHooks::ExplicitWrite& explicitWrite = ClipRecordHooks::explicitWrite();
    std::vector<float> written;
    const bool read = anim::readChannelValue(sceneProject->scene, entity, channel, {}, written);
    if (hadShown && read && !explicitWrite.any() && sameValue(channel, written, shown)){
        return;
    }
    // per component (T3c review C1): a component the command left as it was shown (a lattice
    // point, a blend shape weight, the y of a Position row edit of x) keeps its rest; only what
    // the command changed or named becomes the rest. Rotations stay one value.
    if (hadShown && read && channel != ClipChannel::Rotation && !explicitWrite.all){
        std::vector<float> rest;
        if (restValue(sceneProject->scene, entity, it->second, channel, rest) && rest.size() == written.size() && shown.size() == written.size()){
            bool merged = false;
            for (size_t i = 0; i < written.size(); i++){
                if (written[i] == shown[i] && rest[i] != shown[i] && !explicitWrite.covers(i)){
                    written[i] = rest[i];
                    merged = true;
                }
            }
            if (merged) anim::writeChannelValue(sceneProject->scene, entity, channel, {}, written);
        }
    }
    // the value the command wrote is the new rest; the preview shows again on the next update
    // (not now: the command's other channels are notified after this one)
    takeRest(sceneProject->scene, entity, it->second, channel, true);
}

std::vector<uint32_t> AnimationClipController::previewedKeysOf(Scene* scene, Entity entity, const ClipPropertyKey& property) const{
    std::vector<uint32_t> keys;
    const bool scripts = property.component == ClipRecordHooks::scriptRowComponent();
    size_t scriptIndex = 0;
    std::string field;
    if (scripts && !property.property.empty() && !ClipRecordHooks::scriptRow(property.property, scriptIndex, field)) return keys;
    std::string scriptClass;
    if (scripts && !field.empty()){
        // the row's script: its class names the track component
        const ScriptComponent* component = scene->findComponent<ScriptComponent>(entity);
        if (!component || scriptIndex >= component->scripts.size() || component->scripts[scriptIndex].type != ScriptType::LUA) return keys;
        scriptClass = "script:" + component->scripts[scriptIndex].className;
    }
    for (const std::pair<Entity, uint32_t>& entry : session.writtenProperties){
        if (entry.first != entity) continue;
        const anim::PropertyKeyInfo* info = anim::propertyKeyInfo(entry.second);
        if (!info) continue;
        bool match = false;
        if (scripts){
            if (!info->ref.script) continue;
            if (field.empty()){
                match = true;
            }else if (info->component == scriptClass && info->property == field){
                // the instance the preview bound (two scripts of a class: only its own rows)
                auto rest = session.rest.find(entity);
                const anim::PropertyRest* bound = nullptr;
                if (rest != session.rest.end()){
                    auto found = rest->second.properties.find(entry.second);
                    if (found != rest->second.properties.end()) bound = &found->second;
                }
                match = !bound || bound->binding.scriptIndex < 0 || (size_t)bound->binding.scriptIndex == scriptIndex;
            }
        }else if (property.property.empty()){
            match = info->component == property.component;
        }else{
            match = info->component == property.component && info->property == property.property;
            if (!match){
                // a name the registry spells another way (a swizzle, a parametrized row)
                const anim::PropertyBinding binding = propertyBindingOf(scene, entity, property.component, property.property, 0);
                match = binding.status == anim::PropertyBindStatus::Bound && binding.key == entry.second;
            }
        }
        if (match) keys.push_back(entry.second);
    }
    return keys;
}

void AnimationClipController::beforePropertySceneWrite(uint32_t id, Entity entity, const ClipPropertyKey& property){
    if (!isPreviewing() || id != previewSceneId) return;
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    if (!sceneProject || sceneProject->playState != ScenePlayState::STOPPED || !sceneProject->scene->isEntityCreated(entity)) return;
    // the command reads and edits the rest, not the previewed value
    for (uint32_t key : previewedKeysOf(sceneProject->scene, entity, property)) writePropertyRest(entity, key);
}

void AnimationClipController::afterPropertySceneWrite(uint32_t id, Entity entity, const ClipPropertyKey& property){
    if (!isPreviewing() || id != previewSceneId) return;
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    if (!sceneProject || sceneProject->playState != ScenePlayState::STOPPED || !sceneProject->scene->isEntityCreated(entity)) return;
    auto it = session.rest.find(entity);
    if (it == session.rest.end()) return;
    // the value the command wrote (a named value: the new rest even when equal to what was
    // shown); the preview shows again on the next update
    bool any = false;
    for (uint32_t key : previewedKeysOf(sceneProject->scene, entity, property)){
        auto rest = it->second.properties.find(key);
        if (rest == it->second.properties.end() || !rest->second.has) continue;
        anim::readPropertyRest(sceneProject->scene, entity, rest->second.binding, rest->second);
        any = true;
    }
    if (any) needsApply = true;
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
    const uint64_t clockBefore = clipCommitClock(project);
    std::vector<ClipWarning> warnings;
    // Auto, Linear and Flat keys follow their neighbours (anim-properties: a moved key keeps
    // its mode's meaning)
    ClipData refreshed = data;
    for (ClipTrackData& track : refreshed.tracks) cliptangents::refresh(track);
    if (!edited->commit(refreshed, undoName, mergeKey, error, &warnings)) return false;
    lastWarnings = warnings;
    // the preview shows the new revision through the document's listener; the project step it
    // made (or merged into) is a clip commit (R5b)
    markClipCommit(clockBefore);
    return true;
}

uint64_t AnimationClipController::clipCommitClock(Project* project){
    installTimeline(project);
    return timeline().clock;
}

void AnimationClipController::markClipCommit(uint64_t clockBefore){
    HistoryTimeline& t = timeline();
    CommandHistory* history = historyOf(NULL_PROJECT_SCENE);
    auto steps = t.histories.find(NULL_PROJECT_SCENE);
    if (!history || steps == t.histories.end()) return;
    const size_t position = history->position();
    if (position > 0 && position <= steps->second.stamps.size() && steps->second.stamps[position - 1] > clockBefore){
        steps->second.clip[position - 1] = true;
    }
}

void AnimationClipController::installHistoryTimeline(Project* project){
    HistoryTimeline& t = timeline();
    if (project) t.project = project;
    // the App has just set CommandHistory's listeners (replacing an earlier chain, if any)
    t.installed = true;
    chainTimeline();
}

bool AnimationClipController::newestUndoIsClipCommit(Project* project){
    if (!project) project = timeline().project;
    if (!project || historyOf(NULL_PROJECT_SCENE) != project->getProjectCommandHistory()) return false;
    bool clip = false;
    const uint64_t projectStep = topStep(NULL_PROJECT_SCENE, &clip);
    if (!clip) return false;
    const uint32_t selected = project->getSelectedSceneId();
    const uint64_t sceneStep = selected != NULL_PROJECT_SCENE ? topStep(selected, nullptr) : 0;
    return projectStep > sceneStep;
}

bool AnimationClipController::newestRedoIsClipCommit(Project* project){
    if (!project) project = timeline().project;
    if (!project || historyOf(NULL_PROJECT_SCENE) != project->getProjectCommandHistory()) return false;
    bool clip = false;
    const uint64_t projectStep = redoStep(NULL_PROJECT_SCENE, &clip);
    if (!clip || projectStep == 0) return false;
    const uint32_t selected = project->getSelectedSceneId();
    // newer than the scene's next redo and than its last new step (a scene edit made after the
    // undo is the newest action: Ctrl+Y does not jump back to the clip; scene redos do not count)
    const uint64_t sceneRedo = selected != NULL_PROJECT_SCENE ? redoStep(selected, nullptr) : 0;
    uint64_t sceneNew = 0;
    if (selected != NULL_PROJECT_SCENE){
        auto steps = timeline().histories.find(selected);
        if (steps != timeline().histories.end()) sceneNew = steps->second.lastNew;
    }
    return projectStep > sceneRedo && projectStep > sceneNew;
}

std::shared_ptr<const anim::HumanoidSlots> AnimationClipController::boundHumanoid() const{
    SceneProject* sceneProject = nullptr;
    const Entity target = valueRoot(&sceneProject);
    if (target == NULL_ENTITY || !sceneProject || !sceneProject->scene) return nullptr;
    Scene* scene = sceneProject->scene;
    if (!humanoidCache || humanoidCacheRoot != target || humanoidCacheRevision != scene->hierarchyRevision() ||
        humanoidCachePool != AnimationClipPool::revision()){
        humanoidCache = std::make_shared<anim::HumanoidSlots>(anim::resolveHumanoidSlots(scene, target));
        humanoidCacheRoot = target;
        humanoidCacheRevision = scene->hierarchyRevision();
        humanoidCachePool = AnimationClipPool::revision();
    }
    return humanoidCache->target ? humanoidCache : nullptr;
}

bool AnimationClipController::currentTrackValue(const ClipTrackData& track, std::vector<float>& out) const{
    out.clear();
    SceneProject* sceneProject = nullptr;
    const Entity target = valueRoot(&sceneProject);
    if (target == NULL_ENTITY || !sceneProject) return false;
    Scene* scene = sceneProject->scene;
    if (isHumanoidChannel(track.channel)){
        std::shared_ptr<const anim::HumanoidSlots> humanoid = boundHumanoid();
        return humanoid && anim::readHumanoidValue(scene, *humanoid->target, track.channel, track.humanBone, out);
    }
    const Entity entity = anim::resolvePath(scene, target, track.path);
    if (entity == NULL_ENTITY) return false;
    if (track.channel == ClipChannel::Property) return currentPropertyValue(scene, entity, track, out);
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

bool AnimationClipController::addPropertyTrack(const std::string& path, const std::string& component, const std::string& property,
                                               size_t* newTrack, std::string* error){
    if (!editable(error)) return false;
    std::vector<anim::PathSegment> segments;
    if (!anim::parsePath(path, segments)) return fail(error, "'" + path + "' is not a valid path");
    anim::PropertyRef ref;
    if (!anim::parsePropertyRef(component, property, ref)) return fail(error, ref.error);
    ClipData data = document->data();
    if (data.findPropertyTrack(path, component, property) >= 0){
        return fail(error, "the clip already animates " + path + " : " + component + "." + property);
    }
    SceneProject* sceneProject = nullptr;
    const Entity at = valueRoot(&sceneProject);
    if (at == NULL_ENTITY || !sceneProject) return fail(error, "no GameObject is bound to read the current value from");
    const Entity entity = anim::resolvePath(sceneProject->scene, at, path);
    if (entity == NULL_ENTITY) return fail(error, "no GameObject at '" + path + "' under the bound root");
    if (legacyModelPart(sceneProject->scene, at, entity)){
        return fail(error, "'" + (path.empty() ? sceneProject->scene->getEntityName(entity) : path) + "' " + LEGACY_PART_REASON);
    }
    const anim::PropertyBinding binding = propertyBindingOf(sceneProject->scene, entity, component, property, 0);
    if (binding.status != anim::PropertyBindStatus::Bound) return fail(error, binding.message);
    ClipTrackData track;
    track.path = path;
    track.channel = ClipChannel::Property;
    track.component = component;
    track.property = property;
    // a script field or shader member keeps its bound kind (normalize takes it from the keys)
    track.kind = binding.swizzle >= 0 ? PropertyKind::Float : binding.kind;
    track.width = binding.swizzle >= 0 ? 1 : binding.width;
    std::vector<float> value;
    if (!currentPropertyValue(sceneProject->scene, entity, track, value)) return fail(error, "the current value can't be read");
    setKeyAt(track, time, value);
    data.tracks.push_back(track);
    if (!commitEdit(data, "Add Property", "", error)) return false;
    if (newTrack){
        const int index = document->data().findPropertyTrack(path, component, property);
        *newTrack = index >= 0 ? (size_t)index : 0;
    }
    return true;
}

bool AnimationClipController::addProperty(const std::string& path, ClipChannel channel, size_t* newTrack, std::string* error){
    if (!editable(error)) return false;
    if (channel == ClipChannel::Property) return fail(error, "a property track needs its component and property (addPropertyTrack)");
    if (isHumanoidChannel(channel)){
        // a humanoid track (anim-retarget 9): `path` is its bone; the value from the bound rig
        int bone = -1;
        if (!anim::humanoidTrackBoneFromName(channel, path, bone)) return fail(error, "'" + path + "' is not a humanoid bone");
        ClipData data = document->data();
        if (data.findHumanoidTrack(channel, bone) >= 0) return fail(error, "the clip already animates " + clipui::humanoidTrackLabel(channel, bone));
        ClipTrackData track;
        track.channel = channel;
        track.humanBone = bone;
        std::vector<float> value;
        if (!currentTrackValue(track, value)){
            return fail(error, boundHumanoid() ? "the bound humanoid rig has no " + path
                                               : std::string("bind a humanoid character to key humanoid tracks"));
        }
        setKeyAt(track, time, value);
        data.tracks.push_back(track);
        if (!commitEdit(data, "Add Property", "", error)) return false;
        if (newTrack){
            const int index = document->data().findHumanoidTrack(channel, bone);
            *newTrack = index < 0 ? 0 : (size_t)index;
        }
        return true;
    }
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
    {
        SceneProject* sceneProject = nullptr;
        const Entity at = valueRoot(&sceneProject);
        const Entity entity = sceneProject ? anim::resolvePath(sceneProject->scene, at, path) : NULL_ENTITY;
        const char* refusal = entity != NULL_ENTITY ? legacyRefusal(sceneProject->scene, at, entity, channel) : nullptr;
        if (refusal){
            return fail(error, "'" + (path.empty() ? sceneProject->scene->getEntityName(entity) : path) + "' " + refusal);
        }
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
    return keyTracks(tracks, atTime, mergeKey, nullptr, error);
}

bool AnimationClipController::keyTracks(const std::vector<size_t>& tracks, float atTime, const std::string& mergeKey,
                                        std::vector<KeyRef>* keyed, std::string* error){
    if (keyed) keyed->clear();
    if (!editable(error)) return false;
    if (!finite(atTime) || atTime < 0.0f) return fail(error, "the key time must be 0 or more");
    ClipData data = document->data();
    std::vector<size_t> indices = tracks;
    if (indices.empty()){
        for (size_t i = 0; i < data.tracks.size(); i++) indices.push_back(i);
    }
    std::vector<DragIdentity> written;
    for (size_t index : indices){
        if (index >= data.tracks.size()) return fail(error, "no track " + std::to_string(index));
        ClipTrackData& track = data.tracks[index];
        std::vector<float> value;
        if (!currentTrackValue(track, value)) continue;
        setKeyAt(track, atTime, value);
        written.push_back(identityOf<DragIdentity>(track, atTime));
    }
    if (written.empty()) return fail(error, "no bound track to key (missing bindings are skipped)");
    if (!commitEdit(data, "Add Key", mergeKey, error)) return false;
    // the keyed values are the clip's now: nothing pending for those tracks
    pending.erase(std::remove_if(pending.begin(), pending.end(), [&written](const PendingValue& value){
        return std::any_of(written.begin(), written.end(), [&value](const DragIdentity& identity){
            return identity.path == value.path && identity.channel == value.channel && identity.component == value.component &&
                   identity.property == value.property;
        });
    }), pending.end());
    if (keyed){
        for (const DragIdentity& identity : written){
            const int track = trackOf(document->data(), identity);
            KeyRef ref;
            if (track >= 0 && findKey(document->data(), document->data().tracks[(size_t)track], atTime, ref)) keyed->push_back(ref);
        }
    }
    return true;
}

bool AnimationClipController::keyAll(float atTime, std::string* error){
    return keyTracks({}, atTime, "", nullptr, error);
}

bool AnimationClipController::keyAll(float atTime, std::vector<KeyRef>* keyed, std::string* error){
    return keyTracks({}, atTime, "", keyed, error);
}

bool AnimationClipController::keyModified(float atTime, std::string* error){
    return keyModified(atTime, nullptr, error);
}

bool AnimationClipController::keyModified(float atTime, std::vector<KeyRef>* keyed, std::string* error){
    if (keyed) keyed->clear();
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
        const int built = clipui::findRuntimeTrack(*clip, track);
        if (built < 0 || (size_t)built >= samples.size() || !samples[(size_t)built].valid){
            changed.push_back(i);
            continue;
        }
        if (!sameValue(track.channel, value, sampleValue(samples[(size_t)built]))) changed.push_back(i);
    }
    if (changed.empty()) return fail(error, "no bound channel differs from the clip at the playhead");
    return keyTracks(changed, atTime, "", keyed, error);
}

bool AnimationClipController::setKeyValue(const KeyRef& key, const std::vector<float>& value, const std::string& mergeKey, std::string* error){
    if (!editable(error)) return false;
    ClipData data = document->data();
    if (key.track >= data.tracks.size() || key.key >= data.tracks[key.track].keys.size()) return fail(error, "no such key");
    ClipTrackData& track = data.tracks[key.track];
    for (float v : value){
        if (!finite(v)) return fail(error, "key values must be finite numbers");
    }
    size_t width = ClipData::valueWidth(track.channel, value.size(), track.points.size(), track.width);
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
    // the same rotation on the old value's side: its stored tangents stay valid (no Hermite flip)
    std::vector<float> aligned = value;
    if (track.channel == ClipChannel::Rotation) anim::alignQuaternionKey(aligned, edited.value);
    edited.value = aligned;
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
                if (data.tracks[ref.track].channel == ClipChannel::Rotation && key.value.size() == 4){
                    const float length = std::sqrt(key.value[0] * key.value[0] + key.value[1] * key.value[1] +
                                                   key.value[2] * key.value[2] + key.value[3] * key.value[3]);
                    if (length > 1e-6f){
                        for (float& component : key.value) component /= length;
                    }else{
                        key.value = {0.0f, 0.0f, 0.0f, 1.0f};
                    }
                }
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
        identities.push_back(identityOf<DragIdentity>(track, std::max(0.0f, track.keys[ref.key].time + deltaTime)));
    }
    if (!moveKeyData(data, keys, deltaTime, nullptr, error)) return false;
    if (!commitEdit(data, "Move Keys", mergeKey, error)) return false;
    if (moved){
        moved->clear();
        for (const DragIdentity& identity : identities){
            const int track = trackOf(document->data(), identity);
            if (track < 0) continue;
            const int key = nearestKey(document->data().tracks[(size_t)track], identity.time, 1e-4f);
            if (key < 0) continue;
            // keys that ended on one time collapsed into one (getLastWarnings says so)
            const bool listed = std::any_of(moved->begin(), moved->end(), [&](const KeyRef& ref){
                return ref.track == (size_t)track && ref.key == (size_t)key;
            });
            if (!listed) moved->push_back({(size_t)track, (size_t)key});
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
        identities.push_back(identityOf<DragIdentity>(track, track.keys[ref.key].time));
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
            const int track = trackOf(document->data(), identity);
            if (track < 0) continue;
            const int key = nearestKey(document->data().tracks[(size_t)track], std::max(0.0f, identity.time + totalDeltaTime), 1e-4f);
            if (key < 0) continue;
            // keys that end on one time collapse into one (getLastWarnings says so)
            const bool listed = std::any_of(moved->begin(), moved->end(), [&](const KeyRef& ref){
                return ref.track == (size_t)track && ref.key == (size_t)key;
            });
            if (!listed) moved->push_back({(size_t)track, (size_t)key});
        }
    }
    return true;
}

bool AnimationClipController::updateKeyDrag(float totalDeltaTime, const std::vector<KeyDragValue>& values,
                                            std::vector<KeyRef>* moved, std::string* error){
    if (!dragging) return fail(error, "no key drag in progress");
    if (!editable(error)){
        endKeyDrag();
        return false;
    }
    if (!finite(totalDeltaTime)) return fail(error, "the time offset must be a finite number");
    if (document->revision() != dragRevision){
        endKeyDrag();
        return fail(error, "the clip changed during the drag");
    }
    ClipData data = dragBase;
    // the absolute values first (indices are the drag base's), then the time offset
    bool valued = false;
    for (const KeyDragValue& entry : values){
        if (entry.dragIndex >= dragKeys.size() || entry.value.empty()) continue;
        const KeyRef& ref = dragKeys[entry.dragIndex];
        if (ref.track >= data.tracks.size() || ref.key >= data.tracks[ref.track].keys.size()) return fail(error, "no such key");
        ClipKey& key = data.tracks[ref.track].keys[ref.key];
        if (entry.value.size() != key.value.size()) return fail(error, "a dragged key's value has the wrong width");
        for (float v : entry.value){
            if (!finite(v)) return fail(error, "a dragged key's value must be finite");
        }
        std::vector<float> dragged = entry.value;
        // (on the base value's side: its tangents stay valid, no Hermite flip)
        if (data.tracks[ref.track].channel == ClipChannel::Rotation) anim::alignQuaternionKey(dragged, key.value);
        key.value = dragged;
        if (data.tracks[ref.track].channel == ClipChannel::Rotation){
            float length = 0.0f;
            for (float c : key.value) length += c * c;
            length = std::sqrt(length);
            if (length > 1e-6f){
                for (float& c : key.value) c /= length;
            }else{
                key.value = {0.0f, 0.0f, 0.0f, 1.0f};
            }
        }
        valued = true;
    }
    if (!moveKeyData(data, dragKeys, totalDeltaTime, nullptr, error)) return false;
    if (!commitEdit(data, valued ? "Drag Keys" : "Move Keys", dragMergeKey, error)) return false;
    dragRevision = document->revision();
    if (moved){
        moved->clear();
        for (const DragIdentity& identity : dragIdentities){
            const int track = trackOf(document->data(), identity);
            if (track < 0) continue;
            const int key = nearestKey(document->data().tracks[(size_t)track], std::max(0.0f, identity.time + totalDeltaTime), 1e-4f);
            if (key < 0) continue;
            const bool listed = std::any_of(moved->begin(), moved->end(), [&](const KeyRef& ref){
                return ref.track == (size_t)track && ref.key == (size_t)key;
            });
            if (!listed) moved->push_back({(size_t)track, (size_t)key});
        }
    }
    return true;
}

const ClipData& AnimationClipController::dragBaseData() const{
    return dragBase;
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

bool AnimationClipController::findKey(const ClipData& data, const ClipTrackData& like, float time, KeyRef& out){
    const int track = data.findTrack(like);
    if (track < 0) return false;
    const std::vector<ClipKey>& keys = data.tracks[(size_t)track].keys;
    for (size_t k = 0; k < keys.size(); k++){
        const float difference = keys[k].time - time;
        if (difference <= anim::ANIM_TIME_EPSILON && difference >= -anim::ANIM_TIME_EPSILON){
            out.track = (size_t)track;
            out.key = k;
            return true;
        }
    }
    return false;
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
    if (ease == EaseType::CUSTOM || ease == EaseType::CURVE) return fail(error, "a custom or curve ease can't be stored in a clip");
    ClipData data = document->data();
    std::set<size_t> tracks;
    for (const KeyRef& ref : keys){
        if (ref.track >= data.tracks.size() || ref.key >= data.tracks[ref.track].keys.size()) return fail(error, "no such key");
        data.tracks[ref.track].keys[ref.key].ease = ease;
        tracks.insert(ref.track);
    }
    // tangents win over eases: a track given an ease drops its tangents (the curves become eased)
    // and with them its keys' tangent modes
    for (size_t track : tracks){
        for (ClipKey& key : data.tracks[track].keys){
            key.inTangent.clear();
            key.outTangent.clear();
            key.tangentMode = ClipTangentMode::None;
        }
    }
    return commitEdit(data, "Set Interpolation", "", error);
}

namespace{
    // the notice of eases a track's first tangents replaced (anim-properties)
    void easesNotice(size_t dropped, std::string* notice){
        if (!notice) return;
        notice->clear();
        if (dropped == 0) return;
        *notice = std::to_string(dropped) + (dropped == 1 ? " key's ease" : " keys' eases") +
                  " became linear tangents (a curve with tangents plays them; steps stay)";
    }

    // a track with tangent curves: not a discrete (or unknown) property track (anim-properties'
    // continuous properties have curves)
    bool hasCurve(const ClipTrackData& track){
        return track.channel != ClipChannel::Property || cliptangents::editable(track);
    }
}

bool AnimationClipController::setTangentMode(const std::vector<KeyRef>& keys, ClipTangentMode mode, std::string* error, std::string* notice){
    if (!editable(error)) return false;
    if (mode == ClipTangentMode::None) return fail(error, "no tangent mode given");
    ClipData data = document->data();
    std::set<size_t> tracks;
    for (const KeyRef& ref : keys){
        if (ref.track >= data.tracks.size() || ref.key >= data.tracks[ref.track].keys.size()) return fail(error, "no such key");
        if (!hasCurve(data.tracks[ref.track])) return fail(error, "a discrete property track has no curve");
        ClipKey& key = data.tracks[ref.track].keys[ref.key];
        key.tangentMode = mode;
        // Free: one smooth tangent (the two sides' mean); Broken keeps both
        if (mode == ClipTangentMode::Free && key.hasTangents()){
            for (size_t c = 0; c < key.inTangent.size(); c++){
                const float mean = 0.5f * (key.inTangent[c] + key.outTangent[c]);
                key.inTangent[c] = key.outTangent[c] = mean;
            }
        }
        tracks.insert(ref.track);
    }
    // a Hermite track: the eases go (the step of Constant keys stays), the keys without tangents
    // get them from their modes (applyTangentModes in normalize)
    size_t dropped = 0;
    for (size_t track : tracks){
        for (ClipKey& key : data.tracks[track].keys){
            if (key.tangentMode == ClipTangentMode::Constant) continue;
            if (key.ease != EaseType::LINEAR && key.ease != EaseType::STEP) dropped++;
            key.ease = EaseType::LINEAR;
        }
    }
    if (!commitEdit(data, std::string("Set Tangents: ") + clipui::tangentModeLabel(mode), "", error)) return false;
    easesNotice(dropped, notice);
    return true;
}

bool AnimationClipController::setKeyTangent(const KeyRef& ref, size_t component, bool outSide, float slope, const std::string& mergeKey,
                                            std::string* error){
    if (!editable(error)) return false;
    if (!std::isfinite(slope)) return fail(error, "the tangent is not a finite number");
    ClipData data = document->data();
    if (ref.track >= data.tracks.size() || ref.key >= data.tracks[ref.track].keys.size()) return fail(error, "no such key");
    ClipTrackData& track = data.tracks[ref.track];
    if (track.channel == ClipChannel::Rotation){
        return fail(error, "rotation tangents are quaternion tangents: set a tangent mode instead");
    }
    if (!hasCurve(track)) return fail(error, "a discrete property track has no tangents to edit");
    // the track's keys get their tangents first (a mode on the edited key, normalize the rest)
    ClipKey& key = track.keys[ref.key];
    if (!key.hasTangents()){
        ClipData prepared = data;
        prepared.tracks[ref.track].keys[ref.key].tangentMode = ClipTangentMode::ClampedAuto;
        for (ClipKey& other : prepared.tracks[ref.track].keys){
            if (other.tangentMode != ClipTangentMode::Constant) other.ease = EaseType::LINEAR;
        }
        anim::normalize(prepared);
        KeyRef found;
        if (!findKey(prepared, track, key.time, found)) return fail(error, "no such key");
        // a continuous property track (normalize leaves its tangents): the linear slopes
        cliptangents::ensureTangents(prepared.tracks[found.track]);
        data = prepared;
        return setKeyTangentPrepared(data, found, component, outSide, slope, mergeKey, error);
    }
    return setKeyTangentPrepared(data, ref, component, outSide, slope, mergeKey, error);
}

bool AnimationClipController::setKeyTangentPrepared(ClipData& data, const KeyRef& ref, size_t component, bool outSide, float slope,
                                                    const std::string& mergeKey, std::string* error){
    ClipKey& key = data.tracks[ref.track].keys[ref.key];
    if (component >= key.inTangent.size()) return fail(error, "no component " + std::to_string(component));
    if (key.tangentMode == ClipTangentMode::Broken){
        (outSide ? key.outTangent : key.inTangent)[component] = slope;
    }else{
        key.tangentMode = ClipTangentMode::Free;
        key.inTangent[component] = slope;
        key.outTangent[component] = slope;
    }
    if (key.ease == EaseType::STEP) key.ease = EaseType::LINEAR;
    return commitEdit(data, "Edit Tangent", mergeKey, error);
}

bool AnimationClipController::setSideTangent(const std::vector<KeyRef>& keys, cliptangents::Side side, cliptangents::SideType type,
                                             std::string* error, std::string* notice){
    if (!editable(error)) return false;
    ClipData data = document->data();
    size_t edited = 0, dropped = 0;
    for (const KeyRef& ref : keys){
        if (ref.track >= data.tracks.size() || ref.key >= data.tracks[ref.track].keys.size()) return fail(error, "no such key");
        if (!cliptangents::editable(data.tracks[ref.track])) continue;
        dropped += cliptangents::droppedEases(data.tracks[ref.track]);
        cliptangents::setSide(data.tracks[ref.track], ref.key, side, type);
        edited++;
    }
    if (edited == 0) return fail(error, "rotations and stepped properties have no tangents to edit (use Interpolation)");
    if (!commitEdit(data, "Set Tangents", "", error)) return false;
    easesNotice(dropped, notice);
    return true;
}

bool AnimationClipController::setTangentSlope(const KeyRef& key, size_t component, cliptangents::Side side, float slope,
                                              const std::string& mergeKey, std::string* error, std::string* notice){
    if (!editable(error)) return false;
    const ClipData& data = document->data();
    if (key.track >= data.tracks.size() || key.key >= data.tracks[key.track].keys.size()) return fail(error, "no such key");
    const size_t dropped = cliptangents::droppedEases(data.tracks[key.track]);
    if (!setKeyTangent(key, component, side != cliptangents::Side::Left, slope, mergeKey, error)) return false;
    easesNotice(dropped, notice);
    return true;
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
    ClipTrackData moved = edited;
    moved.path = newPath;
    const int other = data.findTrack(moved);
    if (other >= 0 && (size_t)other != track){
        return fail(error, "the clip already animates " + newPath + " : " +
                    (edited.channel == ClipChannel::Property ? edited.component + "." + edited.property : std::string(anim::channelName(edited.channel))));
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
        listener.animates = [this](uint32_t id, Entity entity, ClipChannel channel){ return recordAnimates(id, entity, channel); };
        listener.claimsProperty = [this](uint32_t id, Entity entity, const ClipPropertyKey& property){
            return recordClaimsProperty(id, entity, property);
        };
        listener.animatesProperty = [this](uint32_t id, Entity entity, const ClipPropertyKey& property){
            return recordAnimatesProperty(id, entity, property);
        };
        recordListener = ClipRecordHooks::addRecordListener(listener);
        return;
    }
    recording = false;
    ClipRecordHooks::removeRecordListener(recordListener);
    recordListener = 0;
    // a source's gesture never outlives Record (T3c review I4)
    ClipRecordHooks::endAllGestures();
}

bool AnimationClipController::isRecording() const{
    return recording;
}

bool AnimationClipController::takeRecordStoppedNotice(){
    const bool notice = recordStoppedNotice;
    recordStoppedNotice = false;
    return notice;
}

// A humanoid clip's Record (anim-retarget 9): the humanoid track a write of `entity`'s `channel`
// keys on the bound rig (a mapped slot or extra bone's rotation, the hips position); false for
// other bones (they key generic tracks)
static bool humanoidRecordTrack(const anim::HumanoidSlots* humanoid, Entity entity, ClipChannel channel, ClipChannel& outChannel, int& outBone){
    if (!humanoid || !humanoid->target) return false;
    const anim::HumanoidTarget& target = *humanoid->target;
    if (channel == ClipChannel::Rotation){
        for (int b = 0; b < pose::HUMAN_BONE_COUNT; b++){
            if (target.slotEntities[(size_t)b] == entity){
                outChannel = ClipChannel::Humanoid;
                outBone = b;
                return true;
            }
        }
        for (int x = 0; x < pose::HUMAN_EXTRA_BONE_COUNT; x++){
            if (target.extraEntities[(size_t)x] == entity){
                outChannel = ClipChannel::Humanoid;
                outBone = pose::HUMAN_BONE_COUNT + x;
                return true;
            }
        }
    }else if (channel == ClipChannel::Position && target.slotEntities[0] == entity){
        outChannel = ClipChannel::HumanoidPosition;
        outBone = HUMANOID_POSITION_HIPS;
        return true;
    }
    return false;
}

bool AnimationClipController::recordClaims(uint32_t id, Entity entity, ClipChannel channel) const{
    // a pure check (the sources ask it every frame): the edit becomes a scene step, and the window
    // says why when one arrives (afterSceneWrite)
    if (!recordEligible(id, entity, channel)) return false;
    return legacyRefusal(sceneProjectOf(previewSceneId)->scene, previewRoot, entity, channel) == nullptr;
}

bool AnimationClipController::recordEligible(uint32_t id, Entity entity, ClipChannel channel) const{
    if (!recording || !isPreviewing() || id != previewSceneId || channel == ClipChannel::Property) return false;
    if (!document || document->isReadOnly() || !document->exists()) return false;
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    if (!sceneProject) return false;
    // anything under the root that has the channel's component: a new track when it has none
    std::string path;
    if (!anim::relativePath(sceneProject->scene, previewRoot, entity, path)) return false;
    // a humanoid clip records on a humanoid character only (its keys are the rig's E)
    if (document->data().isHumanoid() && !boundHumanoid()) return false;
    std::vector<float> value;
    return anim::readChannelValue(sceneProject->scene, entity, channel, {}, value);
}

bool AnimationClipController::recordAnimates(uint32_t id, Entity entity, ClipChannel channel) const{
    if (!recordClaims(id, entity, channel)) return false;
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    std::string path;
    if (!sceneProject || !anim::relativePath(sceneProject->scene, previewRoot, entity, path)) return false;
    ClipChannel humanoidChannel = ClipChannel::Humanoid;
    int bone = -1;
    if (document->data().isHumanoid() && humanoidRecordTrack(boundHumanoid().get(), entity, channel, humanoidChannel, bone)){
        return document->data().findHumanoidTrack(humanoidChannel, bone) >= 0;
    }
    return document->data().findTrack(path, channel) >= 0;
}

bool AnimationClipController::recordClaimsProperty(uint32_t id, Entity entity, const ClipPropertyKey& property) const{
    if (!recording || !isPreviewing() || id != previewSceneId || property.empty()) return false;
    if (!document || document->isReadOnly() || !document->exists()) return false;
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    if (!sceneProject || !sceneProject->scene->isEntityCreated(entity)) return false;
    std::string path;
    if (!anim::relativePath(sceneProject->scene, previewRoot, entity, path)) return false;
    if (legacyModelPart(sceneProject->scene, previewRoot, entity)) return false;
    return propertyBindingOf(sceneProject->scene, entity, property.component, property.property, 0).status == anim::PropertyBindStatus::Bound;
}

bool AnimationClipController::recordAnimatesProperty(uint32_t id, Entity entity, const ClipPropertyKey& property) const{
    if (!recordClaimsProperty(id, entity, property)) return false;
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    std::string path;
    if (!sceneProject || !anim::relativePath(sceneProject->scene, previewRoot, entity, path)) return false;
    // the whole property or one of its components
    for (const ClipTrackData& track : document->data().tracks){
        if (track.channel != ClipChannel::Property || track.path != path || track.component != property.component) continue;
        if (track.property == property.property || track.property.compare(0, property.property.size() + 1, property.property + ".") == 0) return true;
    }
    return false;
}

void AnimationClipController::recordPropertyValue(uint32_t id, Entity entity, const ClipRecordValue& value, const std::string& mergeKey){
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    if (!sceneProject || !recordClaimsProperty(id, entity, value.property)) return;
    {
        const float fps = std::isfinite(document->data().frameRate) && document->data().frameRate >= 1.0f ? document->data().frameRate : 30.0f;
        const float snapped = std::max(0.0f, std::round(time * fps) / fps);
        playing = false;
        if (std::fabs(snapped - time) > 1e-6f) setTime(snapped);
    }
    Scene* scene = sceneProject->scene;
    std::string path;
    if (!anim::relativePath(scene, previewRoot, entity, path)) return;
    const anim::PropertyBinding binding = propertyBindingOf(scene, entity, value.property.component, value.property.property, 0);
    if (binding.status != anim::PropertyBindStatus::Bound || value.value.size() < binding.width) return;

    // the rest is the scene's value before the first live write (stop puts it back)
    float before[4] = {0, 0, 0, 0};
    anim::readPropertyKeySpace(scene, entity, binding, before);
    if (!takePropertyRest(scene, entity, session.rest[entity], binding)) return;
    const std::pair<Entity, uint32_t> entry(entity, binding.key);
    if (std::find(session.writtenProperties.begin(), session.writtenProperties.end(), entry) == session.writtenProperties.end()){
        session.writtenProperties.push_back(entry);
    }
    std::vector<float> keyValue(value.value.begin(), value.value.begin() + binding.width);
    if (binding.discrete || binding.interpolateInt){
        for (float& v : keyValue) v = std::round(v);
    }
    anim::writePropertyKeySpace(scene, entity, binding, keyValue.data());

    // the whole property's track; with only component tracks of it (color.a), those, and new
    // ones for the components this edit changed
    ClipData data = document->data();
    const std::string& component = value.property.component;
    const std::string& property = value.property.property;
    int whole = data.findPropertyTrack(path, component, property);
    std::vector<size_t> parts;
    for (size_t i = 0; i < data.tracks.size(); i++){
        const ClipTrackData& track = data.tracks[i];
        if (track.channel == ClipChannel::Property && track.path == path && track.component == component &&
            track.property.size() == property.size() + 2 && track.property.compare(0, property.size() + 1, property + ".") == 0){
            parts.push_back(i);
        }
    }
    if (whole >= 0 || parts.empty()){
        if (whole < 0){
            ClipTrackData track;
            track.path = path;
            track.channel = ClipChannel::Property;
            track.component = component;
            track.property = property;
            track.kind = binding.kind;
            track.width = binding.width;
            data.tracks.push_back(track);
            whole = (int)data.tracks.size() - 1;
        }
        setKeyAt(data.tracks[(size_t)whole], time, keyValue);
    }else{
        std::vector<bool> keyed(binding.width, false);
        for (size_t i : parts){
            anim::PropertyRef ref;
            anim::parsePropertyRef(component, data.tracks[i].property, ref);
            if (ref.swizzle < 0 || (size_t)ref.swizzle >= binding.width) continue;
            setKeyAt(data.tracks[i], time, {keyValue[(size_t)ref.swizzle]});
            keyed[(size_t)ref.swizzle] = true;
        }
        static const char* const vectorLetters = "xyzw";
        static const char* const colorLetters = "rgba";
        const bool colour = binding.kind == PropertyKind::Color3 || binding.kind == PropertyKind::Color4;
        for (size_t k = 0; k < binding.width; k++){
            if (keyed[k] || std::fabs(keyValue[k] - before[k]) <= 1e-6f) continue;
            ClipTrackData track;
            track.path = path;
            track.channel = ClipChannel::Property;
            track.component = component;
            track.property = property + "." + (colour ? colorLetters[k] : vectorLetters[k]);
            track.kind = PropertyKind::Float;
            track.width = 1;
            setKeyAt(track, time, {keyValue[k]});
            data.tracks.push_back(track);
        }
    }
    std::string error;
    if (commitEdit(data, "Record", mergeKey, &error)) return;
    if (!mergeKey.empty() && mergeKey == failedRecordKey) return;
    failedRecordKey = mergeKey;
    Out::warning("Record: %s", error.c_str());
    ClipRecordHooks::note("Record could not key this edit: " + error);
}

void AnimationClipController::recordValue(uint32_t id, Entity entity, const ClipRecordValue& value, const std::string& mergeKey){
    if (value.channel == ClipChannel::Property){
        recordPropertyValue(id, entity, value, mergeKey);
        return;
    }
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    if (!recordClaims(id, entity, value.channel) || !sceneProject) return;
    // keys land on the Samples grid (T3c review I5); a playing preview pauses there first, so
    // the key and the shown pose stay at one frame
    {
        const float fps = std::isfinite(document->data().frameRate) && document->data().frameRate >= 1.0f ? document->data().frameRate : 30.0f;
        const float snapped = std::max(0.0f, std::round(time * fps) / fps);
        playing = false;
        if (std::fabs(snapped - time) > 1e-6f) setTime(snapped);
    }
    Scene* scene = sceneProject->scene;
    std::string path;
    if (!anim::relativePath(scene, previewRoot, entity, path)) return;

    // the rest is the scene's value before the first live write (stop puts it back)
    takeRest(scene, entity, session.rest[entity], value.channel, false);
    const std::pair<Entity, ClipChannel> entry(entity, value.channel);
    if (std::find(session.written.begin(), session.written.end(), entry) == session.written.end()) session.written.push_back(entry);
    anim::writeChannelValue(scene, entity, value.channel, value.points, value.value);

    ClipData data = document->data();
    // a humanoid clip keys a mapped bone's E (the rig's joint rotation of its new local rotation)
    ClipChannel humanoidChannel = ClipChannel::Humanoid;
    int bone = -1;
    const bool humanoid = data.isHumanoid() && humanoidRecordTrack(boundHumanoid().get(), entity, value.channel, humanoidChannel, bone);
    int index = humanoid ? data.findHumanoidTrack(humanoidChannel, bone) : data.findTrack(path, value.channel);
    if (index < 0){
        ClipTrackData track;
        if (humanoid){
            track.channel = humanoidChannel;
            track.humanBone = bone;
        }else{
            track.path = path;
            track.channel = value.channel;
        }
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
    if (commitEdit(data, "Record", mergeKey, &error)) return;
    // nothing was keyed: said once per gesture (its merge key), in the Console and the window
    if (!mergeKey.empty() && mergeKey == failedRecordKey) return;
    failedRecordKey = mergeKey;
    Out::warning("Record: %s", error.c_str());
    ClipRecordHooks::note("Record could not key this edit: " + error);
}

// ---- App integration ----

void AnimationClipController::stopTransientPreviews(){
    // (a stop deferred by a running save keeps the flag until update() finishes it)
    transientStop = previewing;
    stopPreview();
}

bool AnimationClipController::ownsUndo() const{
    // R5b: by the newest undoable step, whatever window has focus (Record never steals a scene undo)
    return newestStepIsClipEdit();
}

bool AnimationClipController::newestStepIsClipEdit() const{
    return project && newestUndoIsClipCommit(project);
}

bool AnimationClipController::newestRedoIsClipEdit() const{
    return project && newestRedoIsClipCommit(project);
}

const std::vector<ClipWarning>& AnimationClipController::getLastWarnings() const{
    return lastWarnings;
}

bool AnimationClipController::setPendingValue(size_t track, const std::vector<float>& value, std::string* error){
    if (!document) return fail(error, "no clip is open");
    const ClipData& data = document->data();
    if (track >= data.tracks.size()) return fail(error, "no track " + std::to_string(track));
    const ClipTrackData& target = data.tracks[track];
    if (target.channel == ClipChannel::Property && !target.known) return fail(error, "'" + target.component + "." + target.property + "' is not animatable");
    for (float v : value){
        if (!finite(v)) return fail(error, "values must be finite numbers");
    }
    size_t width = ClipData::valueWidth(target.channel, value.size(), target.points.size(), target.width);
    if (target.channel == ClipChannel::Morph){
        if (value.empty() || value.size() > MAX_MORPHTARGETS){
            return fail(error, "a blend shape value holds 1 to " + std::to_string(MAX_MORPHTARGETS) + " weights");
        }
        width = value.size();
    }
    if (value.size() != width){
        return fail(error, "a " + std::string(anim::channelName(target.channel)) + " value holds " + std::to_string(width) + " numbers");
    }
    if (!isPreviewing() && !startPreview(error)) return false;
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    if (!sceneProject) return fail(error, "the previewed scene is gone");
    const Entity entity = anim::resolvePath(sceneProject->scene, previewRoot, target.path);
    std::vector<float> current;
    const bool bound = entity != NULL_ENTITY && (target.channel == ClipChannel::Property
        ? currentPropertyValue(sceneProject->scene, entity, target, current)
        : anim::readChannelValue(sceneProject->scene, entity, target.channel, target.points, current));
    if (!bound){
        return fail(error, "'" + target.path + "' is not bound: nothing shows the value");
    }
    if (pendingTime != time) pending.clear();
    pendingTime = time;
    PendingValue entry;
    entry.path = target.path;
    entry.channel = target.channel;
    entry.points = target.points;
    entry.value = value;
    entry.component = target.component;
    entry.property = target.property;
    pending.erase(std::remove_if(pending.begin(), pending.end(), [&entry](const PendingValue& other){
        return other.path == entry.path && other.channel == entry.channel && other.component == entry.component && other.property == entry.property;
    }), pending.end());
    pending.push_back(entry);
    writePending();
    sceneProject->needUpdateRender = true;
    return true;
}

bool AnimationClipController::hasPendingValue(size_t track) const{
    if (!document || track >= document->data().tracks.size()) return false;
    const ClipTrackData& target = document->data().tracks[track];
    return std::any_of(pending.begin(), pending.end(), [&target](const PendingValue& value){
        return value.path == target.path && value.channel == target.channel && value.component == target.component && value.property == target.property;
    });
}

void AnimationClipController::clearPendingValues(){
    if (pending.empty()) return;
    pending.clear();
    if (isPreviewing()) apply();
}

void AnimationClipController::writePending(){
    SceneProject* sceneProject = sceneProjectOf(previewSceneId);
    if (!sceneProject) return;
    Scene* scene = sceneProject->scene;
    for (const PendingValue& value : pending){
        const Entity entity = anim::resolvePath(scene, previewRoot, value.path);
        if (entity == NULL_ENTITY) continue;
        if (value.channel == ClipChannel::Property){
            // the whole property with the pending components (key space)
            anim::PropertyBinding binding = propertyBindingOf(scene, entity, value.component, value.property, value.value.size());
            if (binding.status != anim::PropertyBindStatus::Bound) continue;
            float whole[4] = {0, 0, 0, 0};
            if (!anim::readPropertyKeySpace(scene, entity, binding, whole)) continue;
            if (!takePropertyRest(scene, entity, session.rest[entity], binding)) continue;
            if (binding.swizzle >= 0){
                if (value.value.empty()) continue;
                whole[binding.swizzle] = value.value[0];
            }else{
                for (size_t k = 0; k < binding.width && k < value.value.size(); k++) whole[k] = value.value[k];
            }
            binding.swizzle = -1;
            const std::pair<Entity, uint32_t> entry(entity, binding.key);
            if (std::find(session.writtenProperties.begin(), session.writtenProperties.end(), entry) == session.writtenProperties.end()){
                session.writtenProperties.push_back(entry);
            }
            anim::writePropertyKeySpace(scene, entity, binding, whole);
            continue;
        }
        // the rest is what the scene had before (stop puts it back)
        takeRest(scene, entity, session.rest[entity], value.channel, false);
        const std::pair<Entity, ClipChannel> entry(entity, value.channel);
        if (std::find(session.written.begin(), session.written.end(), entry) == session.written.end()) session.written.push_back(entry);
        anim::writeChannelValue(scene, entity, value.channel, value.points, value.value);
    }
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
