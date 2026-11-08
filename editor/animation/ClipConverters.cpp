// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipConverters.h"

#include "Project.h"
#include "Scene.h"
#include "animation/AnimationClipIO.h"
#include "animation/ClipBinding.h"
#include "animation/ClipEditorUtil.h"
#include "pose/HumanoidMapIO.h"
#include "pose/ModelRig.h"
#include "animation/ModelSubClips.h"
#include "component/ActionComponent.h"
#include "component/Transform.h"
#include "component/AlphaTweenComponent.h"
#include "component/AnimationComponent.h"
#include "component/ColorTweenComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/LatticeTracksComponent.h"
#include "component/ModelComponent.h"
#include "component/MorphTracksComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/PositionTweenComponent.h"
#include "component/PrefabComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/RotationTweenComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/ScaleTweenComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "component/TranslateTracksComponent.h"
#include "component/TweenComponent.h"
#include "io/FileData.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/MeshSystem.h"
#include "util/AssetDatabase.h"
#include "util/FileUtils.h"

#include "tiny_gltf.h"

#include <algorithm>
#include <cctype>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    bool fail(std::string* error, const std::string& message){
        if (error) *error = message;
        return false;
    }

    void warn(std::vector<ClipWarning>& warnings, const std::string& path, const std::string& message){
        ClipWarning warning;
        warning.path = path;
        warning.message = message;
        warnings.push_back(warning);
    }

    std::string nameOf(Scene* scene, Entity entity){
        return clipui::quotedName(scene, entity);
    }

    // a model in the legacy layout (no node hierarchy): MeshSystem's one rule
    // (needsNodeHierarchyReload without the player condition)
    bool isLegacyLayoutModel(Scene* scene, Entity entity){
        const ModelComponent* model = scene->findComponent<ModelComponent>(entity);
        return model && MeshSystem::isLegacyLayout(*model);
    }

    // the glTF node index a legacy-layout part of `model` was built from, -1 when not one
    int legacyNodeIndex(const ModelComponent& model, Entity entity){
        for (const auto& bone : model.bonesIdMapping){
            if (bone.second == entity) return bone.first;
        }
        for (const auto& node : model.meshNodesMapping){
            if (node.second == entity) return node.first;
        }
        return -1;
    }

    // A legacy-layout model entity that stands in for the glTF scene root (a lone mesh, a skin
    // without its nodes) has that root's matrix in its local transform. The reload a player
    // brings (R3) moves the matrix onto the new root node's entity and leaves the model entity
    // local * root⁻¹ (ModelLoadCmd), so the converted keys of the model entity are taken off it
    // the same way: exact for a root without translation and with a uniform scale (rotation
    // keys * r⁻¹, scale keys / s; positions stay), else kept as they are with a warning.
    // `legacyRoots` receives those model entities.
    // `entity`'s rest scale and every scale key (and tangent) of `path` in `data` are uniform
    bool uniformScales(Scene* scene, Entity entity, const ClipData& data, const std::string& path){
        auto uniform = [](const std::vector<float>& value){
            if (value.size() < 3) return value.empty();
            const float tolerance = 1e-5f * std::max(1.0f, std::fabs(value[0]));
            return std::fabs(value[0] - value[1]) <= tolerance && std::fabs(value[0] - value[2]) <= tolerance;
        };
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform || !uniform({transform->scale.x, transform->scale.y, transform->scale.z})) return false;
        for (const ClipTrackData& track : data.tracks){
            if (track.path != path || track.channel != ClipChannel::Scale) continue;
            for (const ClipKey& key : track.keys){
                if (!uniform(key.value) || !uniform(key.inTangent) || !uniform(key.outTangent)) return false;
            }
        }
        return true;
    }

    void compensateLegacyRootKeys(Scene* scene, Entity root, ClipData& data, std::vector<ClipWarning>& warnings,
                                  std::vector<Entity>& legacyRoots){
        std::shared_ptr<MeshSystem> meshes = scene->getSystem<MeshSystem>();
        if (!meshes) return;
        std::vector<std::string> said;
        for (ClipTrackData& track : data.tracks){
            if (track.channel != ClipChannel::Position && track.channel != ClipChannel::Rotation && track.channel != ClipChannel::Scale) continue;
            const Entity entity = anim::resolvePath(scene, root, track.path);
            if (entity == NULL_ENTITY || !isLegacyLayoutModel(scene, entity)) continue;
            Matrix4 sceneRoot;
            if (!meshes->getLegacyRootTransform(entity, sceneRoot)) continue;
            if (std::find(legacyRoots.begin(), legacyRoots.end(), entity) == legacyRoots.end()) legacyRoots.push_back(entity);
            Vector3 position;
            Vector3 scale;
            Quaternion rotation;
            sceneRoot.decompose(position, scale, rotation);
            const float tolerance = 1e-5f * std::max(1.0f, std::fabs(scale.x));
            // R·S·r⁻¹ = (R·r⁻¹)·S only for a uniform S: with a turned root the model entity's own
            // scale (its rest, every scale key and tangent of the path) must be uniform too
            const bool turned = std::fabs(std::fabs(rotation.w) - 1.0f) > 1e-6f;
            const bool exact = position.length() <= 1e-5f && scale.x > 0.0f && std::fabs(scale.x - scale.y) <= tolerance &&
                               std::fabs(scale.x - scale.z) <= tolerance && !track.relative &&
                               (!turned || uniformScales(scene, entity, data, track.path));
            if (!exact || track.channel == ClipChannel::Position){
                if (exact) continue;
                const bool first = std::find(said.begin(), said.end(), track.path) == said.end();
                if (first){
                    said.push_back(track.path);
                    warn(warnings, track.path, "kept as they are: the model's reload in its node hierarchy moves the file's root "
                         "transform (translated or not uniformly scaled, turned under a non-uniform scale of the model, or the "
                         "track is relative) to its root node, so these "
                         "keys apply it twice; key them again after the reload");
                }
                continue;
            }
            const Quaternion undo = rotation.inverse();
            for (ClipKey& key : track.keys){
                auto adjust = [&](std::vector<float>& value){
                    if (track.channel == ClipChannel::Rotation && value.size() == 4){
                        // linear in q: values and tangents alike
                        quaternionToXYZW(quaternionFromXYZW(value.data()) * undo, value.data());
                    }else if (track.channel == ClipChannel::Scale){
                        for (float& component : value) component /= scale.x;
                    }
                };
                adjust(key.value);
                adjust(key.inTangent);
                adjust(key.outTangent);
            }
            if (std::find(said.begin(), said.end(), track.path) == said.end()){
                said.push_back(track.path);
                warn(warnings, track.path, "its keys are taken off the file's root transform, which the model's reload in its node "
                     "hierarchy moves to its root node");
            }
        }
    }

    // what a track entity animates: the not-converted line of anything else
    std::string actionDescription(Scene* scene, Entity entity){
        const std::string name = nameOf(scene, entity);
        if (scene->findComponent<PositionTweenComponent>(entity)) return "Tween " + name + " (position tween)";
        if (scene->findComponent<RotationTweenComponent>(entity)) return "Tween " + name + " (rotation tween)";
        if (scene->findComponent<ScaleTweenComponent>(entity)) return "Tween " + name + " (scale tween)";
        if (scene->findComponent<ColorTweenComponent>(entity)) return "Tween " + name + " (color tween)";
        if (scene->findComponent<AlphaTweenComponent>(entity)) return "Tween " + name + " (alpha tween)";
        if (scene->findComponent<TweenComponent>(entity)) return "Tween " + name;
        if (scene->findComponent<SpriteAnimationComponent>(entity)) return "Sprite animation " + name;
        if (scene->findComponent<ParticleSystemComponent>(entity)) return "Particles " + name;
        if (scene->findComponent<AnimationComponent>(entity)) return "Animation " + name + " (nested animation)";
        return "Action " + name;
    }

    EaseType easeAt(const KeyframeTracksComponent& keyframe, size_t key){
        if (key >= keyframe.easings.size()) return EaseType::LINEAR;
        const EaseType ease = keyframe.easings[key];
        return ease == EaseType::CUSTOM || ease == EaseType::CURVE ? EaseType::LINEAR : ease;
    }

    // a time or speed in messages ("0.25"): the editor's one short form (clipui)
    std::string clipNumber(float value){
        return clipui::formatSecondsPlain(value);
    }

    // ClipKey values of one entity track (`speed` scales its time line: tangents are per second)
    template<typename T, typename Flatten>
    bool convertKeys(const KeyframeTracksComponent& keyframe, float startTime, float speed,
                     const std::vector<T>& values, const std::vector<T>& inTangents, const std::vector<T>& outTangents,
                     Flatten flatten, ClipTrackData& track, std::vector<ClipWarning>& warnings, const std::string& label){
        const size_t count = std::min(keyframe.times.size(), values.size());
        if (count < keyframe.times.size() || count < values.size()){
            warn(warnings, "", label + ": " + std::to_string(keyframe.times.size()) + " key times but " +
                 std::to_string(values.size()) + " values: the first " + std::to_string(count) + " keys are converted");
        }
        const bool tangents = inTangents.size() == values.size() && outTangents.size() == values.size() && !values.empty();
        bool easedTangents = false;
        for (size_t k = 0; k < count; k++){
            ClipKey key;
            key.time = startTime + keyframe.times[k] / speed;
            flatten(values[k], key.value);
            key.ease = easeAt(keyframe, k);
            if (tangents){
                flatten(inTangents[k], key.inTangent);
                flatten(outTangents[k], key.outTangent);
                for (float& v : key.inTangent) v *= speed;
                for (float& v : key.outTangent) v *= speed;
                if (key.ease != EaseType::LINEAR && k + 1 < count) easedTangents = true;
                key.ease = EaseType::LINEAR;
            }
            track.keys.push_back(key);
        }
        if (easedTangents){
            warn(warnings, "", label + ": a cubic (tangent) track with eased segments: clips use the tangents only, the eases were dropped");
        }
        return count > 0;
    }

    void flattenVector(const Vector3& value, std::vector<float>& out){
        out = {value.x, value.y, value.z};
    }

    void flattenQuaternion(const Quaternion& value, std::vector<float>& out){
        out.resize(4);
        quaternionToXYZW(value, out.data());
    }

    void flattenWeights(const std::vector<float>& value, std::vector<float>& out){
        out = value;
        if (out.size() > (size_t)MAX_MORPHTARGETS) out.resize(MAX_MORPHTARGETS);
    }

    void flattenOffsets(const std::vector<Vector3>& value, std::vector<float>& out){
        out.clear();
        for (const Vector3& offset : value){
            out.push_back(offset.x);
            out.push_back(offset.y);
            out.push_back(offset.z);
        }
    }

    const float TIME_EPSILON = anim::ANIM_TIME_EPSILON * 10.0f;
    // the width of the jump a looping track makes where its cycle restarts inside the clip's
    // cycle (keys closer than 1e-6 s merge: the clip can't jump in zero time)
    const float WRAP_JUMP = 1e-5f;

    // One converted track's raw value at `time` (its own keys, sampled as the runtime does)
    bool sampleTrackValue(const ClipTrackData& track, float time, std::vector<float>& out){
        ClipData single;
        single.tracks.push_back(track);
        single.tracks.back().loop = false;
        single.tracks.back().relative = false;
        std::shared_ptr<const AnimationClip> clip = AnimationClip::build(single);
        if (clip->getTracks().empty()) return false;
        anim::TrackSample sample;
        if (!anim::sampleTrack(clip->getTracks()[0], time, sample)) return false;
        out.clear();
        switch (track.channel){
            case ClipChannel::Position:
            case ClipChannel::Scale:
            case ClipChannel::HumanoidPosition:
                out = {sample.vector.x, sample.vector.y, sample.vector.z};
                break;
            case ClipChannel::Rotation:
            case ClipChannel::Humanoid:
                out.resize(4);
                quaternionToXYZW(sample.rotation, out.data());
                break;
            case ClipChannel::Morph:
                out = sample.weights;
                break;
            case ClipChannel::Lattice:
                for (const Vector3& offset : sample.offsets){
                    out.push_back(offset.x);
                    out.push_back(offset.y);
                    out.push_back(offset.z);
                }
                break;
            case ClipChannel::Property:
                return false;
        }
        return true;
    }

    bool sameValue(const ClipKey& a, const ClipKey& b, ClipChannel channel){
        if (a.value.size() != b.value.size()) return false;
        bool same = true;
        bool negated = channel == ClipChannel::Rotation;   // q and -q are one rotation
        for (size_t i = 0; i < a.value.size(); i++){
            same = same && std::fabs(a.value[i] - b.value[i]) <= 1e-6f;
            negated = negated && std::fabs(a.value[i] + b.value[i]) <= 1e-6f;
        }
        return same || negated;
    }

    // A looping track whose frame starts at `start` (> 0): the entity loops its keys over their
    // own period P from `start` on (ActionSystem: fmod of the track time), the clip's looping
    // track from clip time 0. The keys rotated by start mod P over [0, P] make the clip's loop
    // the entity's from `start` on (exactly, where the cycle point falls on a key or in a linear,
    // stepped or non-rotation cubic segment). Before `start` the clip already plays the cycle.
    // False with `reason` when it can't be represented.
    bool rotateLoopTrack(ClipTrackData& track, float start, std::string& reason){
        std::vector<ClipKey> keys = track.keys;
        for (ClipKey& key : keys) key.time -= start;
        const float period = keys.back().time;
        track.keys = keys;
        if (period <= TIME_EPSILON) return true;
        if (keys.front().time > TIME_EPSILON){
            // before its first key the track shows that key
            if (keys.front().hasTangents()){
                reason = "a cubic looping track whose first key is after 0 can't be offset in a clip";
                return false;
            }
            ClipKey first = keys.front();
            first.time = 0.0f;
            first.ease = EaseType::STEP;
            keys.insert(keys.begin(), first);
        }
        const double phase = std::fmod((double)start, (double)period);
        if (phase <= TIME_EPSILON || period - phase <= TIME_EPSILON){
            track.keys = keys;
            return true;
        }
        const float cut = (float)(period - phase);    // the track time at clip time 0
        const float shift = (float)phase;
        size_t j = 0;
        while (j + 1 < keys.size() && keys[j + 1].time <= cut + TIME_EPSILON) j++;

        ClipKey split;
        if (std::fabs(keys[j].time - cut) <= TIME_EPSILON){
            split = keys[j];
        }else{
            const ClipKey& from = keys[j];
            const ClipKey& to = keys[j + 1];
            const bool cubic = from.hasTangents() && to.hasTangents();
            if (cubic && track.channel == ClipChannel::Rotation){
                reason = "its cycle point falls inside a cubic rotation segment, which can't be split exactly";
                return false;
            }
            if (!cubic && from.ease != EaseType::LINEAR && from.ease != EaseType::STEP){
                reason = "its cycle point falls inside an eased segment, which can't be split exactly";
                return false;
            }
            split = from;
            split.time = cut;
            if (from.ease == EaseType::STEP && !cubic){
                split.value = from.value;
            }else if (!sampleTrackValue(track, cut, split.value)){
                reason = "it can't be sampled";
                return false;
            }
            if (cubic){
                // the derivative of the segment's Hermite curve at the cut (units per second)
                const float h = to.time - from.time;
                const float u = (cut - from.time) / h;
                const float d00 = 6 * u * u - 6 * u, d10 = 3 * u * u - 4 * u + 1, d01 = -6 * u * u + 6 * u, d11 = 3 * u * u - 2 * u;
                split.inTangent.assign(from.value.size(), 0.0f);
                for (size_t i = 0; i < from.value.size(); i++){
                    split.inTangent[i] = (d00 * from.value[i] + d10 * h * from.outTangent[i] + d01 * to.value[i] + d11 * h * to.inTangent[i]) / h;
                }
                split.outTangent = split.inTangent;
            }
        }

        std::vector<ClipKey> rotated;
        ClipKey head = split;
        head.time = 0.0f;
        rotated.push_back(head);
        for (size_t k = j + 1; k < keys.size(); k++){
            ClipKey key = keys[k];
            key.time -= cut;
            rotated.push_back(key);
        }
        // the entity's own wrap: from the last key back to the first, at clip time `shift`
        ClipKey& last = rotated.back();
        ClipKey first = keys.front();
        first.time = shift;
        if (sameValue(last, first, track.channel)){
            first.inTangent = last.inTangent;
            rotated.back() = first;
        }else{
            last.ease = EaseType::STEP;
            first.time = shift + WRAP_JUMP;
            rotated.push_back(first);
        }
        for (size_t k = 1; k <= j; k++){
            ClipKey key = keys[k];
            key.time += shift;
            if (std::fabs(keys[k].time - cut) <= TIME_EPSILON) break;   // the split key itself
            rotated.push_back(key);
        }
        ClipKey tail = split;
        tail.time = (float)period;
        rotated.push_back(tail);
        track.keys = rotated;
        return true;
    }

    // the absolute file of a clip or model path as callers give it (absolute, project-relative
    // or asset-relative)
    fs::path absoluteFile(Project* project, const std::string& path){
        const fs::path given = editor::FileUtils::pathFromUtf8(path);
        if (given.is_absolute() || !project) return given.lexically_normal();
        std::error_code ec;
        const fs::path inProject = (project->getProjectPath() / given).lexically_normal();
        if (fs::exists(inProject, ec)) return inProject;
        const fs::path inAssets = project->resolveAssetPath(given);
        if (fs::exists(inAssets, ec)) return inAssets;
        return inProject;
    }

}

Entity ClipConverters::defaultConvertRoot(Scene* scene, Entity animation){
    if (!scene || animation == NULL_ENTITY || !scene->isEntityCreated(animation)) return NULL_ENTITY;
    const ActionComponent* own = scene->findComponent<ActionComponent>(animation);
    if (own && own->target != NULL_ENTITY && scene->isEntityCreated(own->target)) return own->target;
    const AnimationComponent* component = scene->findComponent<AnimationComponent>(animation);
    if (!component) return NULL_ENTITY;
    // the lowest common Transform ancestor (or self) of the keyframe tracks' targets
    std::vector<Entity> common;
    bool first = true;
    for (const ActionFrame& frame : component->actions){
        if (frame.action == NULL_ENTITY || !scene->isEntityCreated(frame.action)) continue;
        if (!scene->findComponent<KeyframeTracksComponent>(frame.action)) continue;
        const ActionComponent* action = scene->findComponent<ActionComponent>(frame.action);
        if (!action || action->target == NULL_ENTITY || !scene->isEntityCreated(action->target)) continue;
        std::vector<Entity> chain;   // the topmost ancestor first
        for (Entity current = action->target; current != NULL_ENTITY && chain.size() < 65536;){
            chain.insert(chain.begin(), current);
            const Transform* transform = scene->findComponent<Transform>(current);
            current = transform ? transform->parent : NULL_ENTITY;
        }
        if (first){
            common = chain;
            first = false;
        }else{
            size_t n = 0;
            while (n < common.size() && n < chain.size() && common[n] == chain[n]) n++;
            common.resize(n);
        }
    }
    return common.empty() ? NULL_ENTITY : common.back();
}

bool ClipConverters::clipPath(Scene* scene, Entity root, Entity entity, std::string& out){
    out.clear();
    if (!scene || root == NULL_ENTITY || entity == NULL_ENTITY || !scene->isEntityCreated(root) || !scene->isEntityCreated(entity)){
        return false;
    }
    // the nearest legacy-layout model part on the way up: its node-hierarchy path
    std::vector<Entity> chain;   // entity .. root
    Entity current = entity;
    for (int depth = 0; current != NULL_ENTITY && depth < 65536; depth++){
        chain.push_back(current);
        if (current == root) break;
        const Transform* transform = scene->findComponent<Transform>(current);
        current = transform ? transform->parent : NULL_ENTITY;
    }
    if (chain.empty() || chain.back() != root){
        return false;
    }
    for (size_t i = 0; i < chain.size(); i++){
        const Entity part = chain[i];
        for (size_t m = i + 1; m < chain.size(); m++){
            const Entity modelEntity = chain[m];
            if (!isLegacyLayoutModel(scene, modelEntity)) continue;
            const ModelComponent& model = scene->getComponent<ModelComponent>(modelEntity);
            const int node = legacyNodeIndex(model, part);
            if (node < 0 || !model.gltfModel) continue;
            const std::vector<std::string> nodePaths = anim::modelNodePaths(*model.gltfModel);
            if (node >= (int)nodePaths.size() || nodePaths[node].empty()) continue;
            std::string prefix;
            if (!anim::relativePath(scene, root, modelEntity, prefix)) return false;
            std::string below;
            if (part != entity && !anim::relativePath(scene, part, entity, below)) return false;
            out = prefix;
            out += (out.empty() ? "" : "/") + nodePaths[node];
            if (!below.empty()) out += "/" + below;
            return true;
        }
    }
    return anim::relativePath(scene, root, entity, out);
}

bool ClipConverters::convertEntityAnimation(Scene* scene, Entity animation, Entity root, ClipData& out,
                                            ClipConvertReport& report, std::string* error){
    out = ClipData();
    report = ClipConvertReport();
    if (!scene || animation == NULL_ENTITY || !scene->isEntityCreated(animation)){
        return fail(error, "no Animation to convert");
    }
    const AnimationComponent* animationComponent = scene->findComponent<AnimationComponent>(animation);
    if (!animationComponent){
        return fail(error, nameOf(scene, animation) + " is not an Animation (no AnimationComponent)");
    }
    if (root == NULL_ENTITY){
        root = defaultConvertRoot(scene, animation);
    }
    if (root == NULL_ENTITY || !scene->isEntityCreated(root)){
        return fail(error, "the Animation " + nameOf(scene, animation) + " has no target: choose the root its clip plays on");
    }

    const std::vector<ActionFrame> frames = animationComponent->actions;
    const bool loop = animationComponent->loop;
    const float duration = animationComponent->duration;
    std::shared_ptr<ActionSystem> actions = scene->getSystem<ActionSystem>();

    float actionsEnd = 0.0f;   // the loop length of an Animation without a duration
    for (const ActionFrame& frame : frames){
        if (frame.action == NULL_ENTITY || !scene->isEntityCreated(frame.action)) continue;
        const ActionComponent* action = scene->findComponent<ActionComponent>(frame.action);
        const float speed = action ? action->speed : 1.0f;
        if (actions && speed > 0.0f){
            actionsEnd = std::max(actionsEnd, frame.startTime + actions->getFrameDuration(frame) / speed);
        }
    }
    // where a looping Animation starts over (the clip's length)
    const float cycle = duration >= 0.0f ? duration : actionsEnd;

    for (const ActionFrame& frame : frames){
        const Entity entity = frame.action;
        if (entity == NULL_ENTITY || !scene->isEntityCreated(entity)){
            report.notConverted.push_back("Action (missing GameObject)");
            continue;
        }
        const ActionComponent* action = scene->findComponent<ActionComponent>(entity);
        const float speed = action ? action->speed : 1.0f;

        const KeyframeTracksComponent* keyframe = scene->findComponent<KeyframeTracksComponent>(entity);
        const bool isTrack = keyframe && (scene->findComponent<TranslateTracksComponent>(entity) ||
                                          scene->findComponent<RotateTracksComponent>(entity) ||
                                          scene->findComponent<ScaleTracksComponent>(entity) ||
                                          scene->findComponent<MorphTracksComponent>(entity) ||
                                          scene->findComponent<LatticeTracksComponent>(entity));
        if (!isTrack){
            report.notConverted.push_back(actionDescription(scene, entity));
            continue;
        }
        const std::string label = "Track " + nameOf(scene, entity);
        const Entity target = action ? action->target : NULL_ENTITY;
        if (target == NULL_ENTITY || !scene->isEntityCreated(target)){
            report.notConverted.push_back(label + " (no target)");
            continue;
        }
        if (speed <= 0.0f){
            report.notConverted.push_back(label + " (speed " + clipNumber(speed) + ": only forward speeds convert)");
            continue;
        }
        std::string path;
        if (!clipPath(scene, root, target, path)){
            report.notConverted.push_back(label + " (its target " + nameOf(scene, target) + " is not " + nameOf(scene, root) +
                                          " or under it, or a name on the way has '/' or ends in #<digits>)");
            continue;
        }

        size_t before = out.tracks.size();
        auto addTrack = [&](ClipChannel channel){
            ClipTrackData track;
            track.path = path;
            track.channel = channel;
            track.loop = keyframe->loop;
            track.relative = keyframe->relative && (channel == ClipChannel::Position || channel == ClipChannel::Rotation ||
                                                    channel == ClipChannel::Scale);
            return track;
        };
        if (const TranslateTracksComponent* tracks = scene->findComponent<TranslateTracksComponent>(entity)){
            ClipTrackData track = addTrack(ClipChannel::Position);
            if (convertKeys(*keyframe, frame.startTime, speed, tracks->values, tracks->inTangents, tracks->outTangents,
                            flattenVector, track, report.warnings, label)) out.tracks.push_back(track);
        }
        if (const RotateTracksComponent* tracks = scene->findComponent<RotateTracksComponent>(entity)){
            ClipTrackData track = addTrack(ClipChannel::Rotation);
            if (convertKeys(*keyframe, frame.startTime, speed, tracks->values, tracks->inTangents, tracks->outTangents,
                            flattenQuaternion, track, report.warnings, label)) out.tracks.push_back(track);
        }
        if (const ScaleTracksComponent* tracks = scene->findComponent<ScaleTracksComponent>(entity)){
            ClipTrackData track = addTrack(ClipChannel::Scale);
            if (convertKeys(*keyframe, frame.startTime, speed, tracks->values, tracks->inTangents, tracks->outTangents,
                            flattenVector, track, report.warnings, label)) out.tracks.push_back(track);
        }
        if (const MorphTracksComponent* tracks = scene->findComponent<MorphTracksComponent>(entity)){
            ClipTrackData track = addTrack(ClipChannel::Morph);
            if (convertKeys(*keyframe, frame.startTime, speed, tracks->values, tracks->inTangents, tracks->outTangents,
                            flattenWeights, track, report.warnings, label)) out.tracks.push_back(track);
        }
        if (const LatticeTracksComponent* tracks = scene->findComponent<LatticeTracksComponent>(entity)){
            ClipTrackData track = addTrack(ClipChannel::Lattice);
            track.points = tracks->points;
            // every key lists the track's points (a short row holds 0 for the rest)
            std::vector<std::vector<Vector3>> values = tracks->values;
            for (std::vector<Vector3>& row : values) row.resize(tracks->points.size(), Vector3(0, 0, 0));
            std::vector<std::vector<Vector3>> inTangents = tracks->inTangents;
            std::vector<std::vector<Vector3>> outTangents = tracks->outTangents;
            for (std::vector<Vector3>& row : inTangents) row.resize(tracks->points.size(), Vector3(0, 0, 0));
            for (std::vector<Vector3>& row : outTangents) row.resize(tracks->points.size(), Vector3(0, 0, 0));
            if (!tracks->points.empty() &&
                convertKeys(*keyframe, frame.startTime, speed, values, inTangents, outTangents, flattenOffsets, track, report.warnings, label)){
                out.tracks.push_back(track);
            }
        }
        // before its frame starts the entity track leaves the target as it is
        const float start = frame.startTime;
        bool dropped = false;
        for (size_t t = before; t < out.tracks.size(); ){
            ClipTrackData& track = out.tracks[t];
            if (track.keys.empty() || start <= TIME_EPSILON){
                t++;
                continue;
            }
            if (track.loop){
                // the entity loops the keys from `start` on: the clip's loop rotated to that phase
                std::string reason;
                if (!rotateLoopTrack(track, start, reason)){
                    report.notConverted.push_back(label + " (" + anim::channelName(track.channel) + " loops from " +
                                                  clipNumber(start) + " s: " + reason + ")");
                    out.tracks.erase(out.tracks.begin() + (long)t);
                    dropped = true;
                    continue;
                }
                warn(report.warnings, "", label + ": loops from " + clipNumber(start) + " s; before that the clip already "
                     "plays its cycle where the entity Animation shows the scene value" + std::string(loop ? " (every cycle)" : ""));
                t++;
                continue;
            }
            if (track.keys.front().hasTangents()){
                warn(report.warnings, "", label + ": starts at " + clipNumber(start) +
                     " s; a cubic track holds its first key from 0 in the clip");
                t++;
                continue;
            }
            // from the frame's start the track shows key 0 until key 0's own time
            if (track.keys.front().time > start + TIME_EPSILON){
                ClipKey first = track.keys.front();
                first.time = start;
                first.ease = EaseType::STEP;
                track.keys.insert(track.keys.begin(), first);
            }
            ClipKey hold;
            hold.time = 0.0f;
            hold.ease = EaseType::STEP;
            bool held = false;
            if (loop && cycle > start){
                // every cycle but the first starts from what the previous one ended with
                held = sampleTrackValue(track, cycle, hold.value);
                if (held){
                    warn(report.warnings, "", label + ": starts at " + clipNumber(start) + " s in a looping Animation; before "
                         "that the clip holds the value the cycle ends with (the entity Animation shows the scene value "
                         "instead in its first cycle)");
                }
            }else if (track.relative){
                if (track.channel == ClipChannel::Position) hold.value = {0.0f, 0.0f, 0.0f};
                else if (track.channel == ClipChannel::Rotation) hold.value = {0.0f, 0.0f, 0.0f, 1.0f};
                else hold.value = {1.0f, 1.0f, 1.0f};
                held = true;
            }else{
                held = anim::readChannelValue(scene, target, track.channel, track.points, hold.value);
            }
            if (held){
                if (track.channel == ClipChannel::Morph) hold.value.resize(track.keys.front().value.size(), 0.0f);
                track.keys.insert(track.keys.begin(), hold);
            }
            t++;
        }
        if (out.tracks.size() > before){
            report.convertedTracks += out.tracks.size() - before;
            // an entity with a channel left out stays (Remove the converted tracks keeps it)
            if (!dropped) report.convertedTrackEntities.push_back(entity);
        }else if (!dropped){
            report.notConverted.push_back(label + " (no keys)");
        }
    }

    if (report.convertedTracks == 0){
        std::string message = "the Animation " + nameOf(scene, animation) + " has no keyframe tracks to convert";
        if (!report.notConverted.empty()){
            message += " (not converted: " + report.notConverted[0] + (report.notConverted.size() > 1 ? ", ..." : "") + ")";
        }
        return fail(error, message);
    }

    // the keys of a legacy model entity carrying the file's root transform (its reload moves it)
    compensateLegacyRootKeys(scene, root, out, report.warnings, report.legacyRootModels);

    out.wrap = loop ? ClipWrapMode::Loop : ClipWrapMode::Once;
    out.source.kind = "entity";
    out.source.name = scene->getEntityName(animation);
    anim::normalize(out, &report.warnings);

    // the length the Animation ends (or loops) at, when it is not the last key
    float length = -1.0f;
    if (duration >= 0.0f){
        length = duration;
    }else if (loop && actionsEnd > 0.0f){
        length = actionsEnd;
    }
    if (length >= 0.0f && std::fabs(length - out.effectiveLength()) > 1e-6f){
        out.length = length;
    }
    if (const ActionComponent* action = scene->findComponent<ActionComponent>(animation)){
        if (action->speed != 1.0f){
            warn(report.warnings, "", "the Animation plays at speed " + clipNumber(action->speed) +
                 ": the clip keeps its own times (set the Animation Player's speed)");
        }
    }
    return true;
}

bool ClipConverters::extractModelSubClip(const std::string& reference, ClipData& out,
                                         std::vector<ClipWarning>& warnings, std::string* error){
    out = ClipData();
    if (!anim::isSubClipReference(reference)){
        return fail(error, "'" + reference + "' is not a model clip (\"<model>#<animation>\", e.g. models/hero.gltf#Walk)");
    }
    std::string failure;
    if (!anim::buildModelSubClip(reference, out, warnings, &failure)){
        out = ClipData();
        return fail(error, failure.empty() ? "can't read '" + reference + "'" : failure);
    }
    if (out.source.empty()){
        anim::SubClipReference parsed;
        anim::parseSubClipReference(reference, parsed);
        out.source.kind = "model";
        out.source.path = parsed.model;
        out.source.name = parsed.clip;
    }
    // a model whose Animation Type is Humanoid serves humanoid clips: its extract is humanoid too
    anim::SubClipReference parsed;
    if (anim::parseSubClipReference(reference, parsed) && isHumanoidModel(parsed.model)){
        ClipData converted;
        std::string failure;
        if (convertToHumanoid(out, parsed.model, converted, warnings, &failure)){
            out = converted;
        }else{
            warnings.push_back({"", "the model's Animation Type is Humanoid, but " + failure + ": extracted as a generic clip"});
        }
    }
    // a new asset (the model is not touched)
    out.guid = anim::generateClipGuid();
    return true;
}

bool ClipConverters::isHumanoidModel(const std::string& modelAsset){
    std::string stamp;
    if (!pose::humanoidSidecarStamp(modelAsset, stamp, nullptr) || stamp.empty()) return false;
    pose::HumanoidOverrides overrides;
    std::vector<std::string> warnings;
    return pose::parseHumanoidOverrides(stamp, overrides, warnings, nullptr) && overrides.isHumanoid();
}

std::string ClipConverters::humanoidSourceModel(const std::string& reference, const ClipData& data){
    anim::SubClipReference parsed;
    if (anim::parseSubClipReference(reference, parsed)) return parsed.model;
    if (data.source.kind == "model" && anim::isModelFilePath(data.source.path)) return data.source.path;
    if (anim::isModelFilePath(data.previewModel)) return data.previewModel;
    return std::string();
}

bool ClipConverters::convertToHumanoid(const ClipData& source, const std::string& modelAsset, ClipData& out,
                                       std::vector<ClipWarning>& warnings, std::string* error, anim::HumanoidConversionReport* report){
    out = ClipData();
    if (modelAsset.empty()) return fail(error, "the clip names no source model (set its Preview Model)");
    if (!anim::isModelFilePath(modelAsset)) return fail(error, "'" + modelAsset + "' is not a model (.gltf, .glb or .fbx)");
    // the rig as the files are now (a sidecar or model edited since is read again)
    anim::HumanoidSourceCache::drop(modelAsset);
    std::shared_ptr<const anim::HumanoidSource> rig;
    std::string failure;
    if (!anim::HumanoidSourceCache::get(modelAsset, rig, &failure)) return fail(error, failure);
    if (!anim::convertToHumanoid(source, *rig, out, warnings, &failure, report)) return fail(error, failure);
    if (out.previewModel.empty()) out.previewModel = modelAsset;
    out.guid = anim::generateClipGuid();
    return true;
}

bool ClipConverters::planUseAnimationPlayer(Scene* scene, Entity model, UseAnimationPlayerPlan& out, std::string* error){
    out = UseAnimationPlayerPlan();
    out.model = model;
    if (!scene || model == NULL_ENTITY || !scene->isEntityCreated(model)){
        return fail(error, "no model GameObject");
    }
    const ModelComponent* component = scene->findComponent<ModelComponent>(model);
    if (!component || component->filename.empty()){
        return fail(error, nameOf(scene, model) + " is not a model GameObject");
    }
    if (!anim::isModelFilePath(component->filename)){
        return fail(error, "'" + component->filename + "' has no animations an Animation Player can play (.gltf, .glb or .fbx)");
    }
    const std::string filename = component->filename;
    std::vector<anim::ModelClipInfo> clips;
    std::string failure;
    if (!anim::listModelFileClips(filename, clips, &failure)){
        return fail(error, failure.empty() ? "can't read '" + filename + "'" : failure);
    }
    if (clips.empty()){
        return fail(error, "'" + filename + "' has no animations");
    }

    std::vector<AnimationPlayerClip> existing;
    if (const AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(model)){
        out.addPlayer = false;
        existing = player->clips;
    }
    for (const anim::ModelClipInfo& clip : clips){
        AnimationPlayerClip entry;
        entry.clip = anim::formatSubClipReference(filename, clip.name);
        bool listed = false;
        for (const AnimationPlayerClip& other : existing){
            listed = listed || other.clip == entry.clip || other.clip == anim::formatSubClipReference(filename, std::to_string(clip.index));
        }
        if (!listed) out.clips.push_back(entry);
    }

    // the entity animations and their tracks
    const ModelComponent& current = scene->getComponent<ModelComponent>(model);
    for (Entity animation : current.animations){
        if (animation == NULL_ENTITY || !scene->isEntityCreated(animation)) continue;
        out.removeEntities.push_back(animation);
        if (const AnimationComponent* animationComponent = scene->findComponent<AnimationComponent>(animation)){
            for (const ActionFrame& frame : animationComponent->actions){
                if (frame.action != NULL_ENTITY && scene->isEntityCreated(frame.action) &&
                    std::find(out.removeEntities.begin(), out.removeEntities.end(), frame.action) == out.removeEntities.end()){
                    out.removeEntities.push_back(frame.action);
                }
            }
        }
    }
    out.reloadModel = isLegacyLayoutModel(scene, model);
    if (out.clips.empty() && out.removeEntities.empty() && !current.animationEntities && !out.addPlayer && !out.reloadModel){
        return fail(error, nameOf(scene, model) + " already uses its Animation Player for every animation of '" + filename + "'");
    }
    return true;
}

bool ClipConverters::bakeChannels(Scene* scene, Entity root, const std::vector<BakedChannel>& channels, float length, bool loop,
                                  ClipData& out, std::string* error){
    out = ClipData();
    if (!scene || root == NULL_ENTITY || !scene->isEntityCreated(root)){
        return fail(error, "no root to bake on");
    }
    if (channels.empty()){
        return fail(error, "nothing to bake");
    }
    for (size_t c = 0; c < channels.size(); c++){
        const BakedChannel& channel = channels[c];
        if (channel.channel == ClipChannel::Property){
            return fail(error, "property channels can't be baked");
        }
        if (channel.times.size() != channel.values.size() || channel.times.empty()){
            return fail(error, "channel " + std::to_string(c) + ": " + std::to_string(channel.times.size()) + " times but " +
                        std::to_string(channel.values.size()) + " values");
        }
        ClipTrackData track;
        track.channel = channel.channel;
        if (!clipPath(scene, root, channel.target, track.path)){
            return fail(error, "channel " + std::to_string(c) + ": " +
                        (scene->isEntityCreated(channel.target) ? nameOf(scene, channel.target) : std::string("its target")) +
                        " is not " + nameOf(scene, root) + " or under it, or a name on the way can't be addressed");
        }
        const size_t width = channel.channel == ClipChannel::Morph ? 0 : ClipData::valueWidth(channel.channel);
        for (size_t k = 0; k < channel.times.size(); k++){
            if (width && channel.values[k].size() != width){
                return fail(error, "channel " + std::to_string(c) + " key " + std::to_string(k) + ": " +
                            std::to_string(channel.values[k].size()) + " values, expected " + std::to_string(width));
            }
            ClipKey key;
            key.time = channel.times[k];
            key.value = channel.values[k];
            key.ease = channel.ease == EaseType::CUSTOM || channel.ease == EaseType::CURVE ? EaseType::LINEAR : channel.ease;
            track.keys.push_back(key);
        }
        out.tracks.push_back(track);
    }
    out.wrap = loop ? ClipWrapMode::Loop : ClipWrapMode::Once;
    std::vector<ClipWarning> warnings;
    anim::normalize(out, &warnings);
    if (length >= 0.0f && std::fabs(length - out.effectiveLength()) > 1e-6f){
        out.length = length;
    }
    return true;
}

std::string ClipConverters::assetRelative(Project* project, const fs::path& path){
    if (!project) return FileUtils::pathToGenericUtf8(path);
    return FileUtils::pathToGenericUtf8(project->normalizeToAssetsRelative(path));
}

AnimationPlayerClip ClipConverters::playerEntry(Project* project, const std::string& clip){
    AnimationPlayerClip entry;
    anim::SubClipReference reference;
    if (anim::parseSubClipReference(clip, reference)){
        const std::string model = assetRelative(project, absoluteFile(project, reference.model));
        entry.clip = anim::formatSubClipReference(model, reference.clip);
        return entry;
    }
    const fs::path file = absoluteFile(project, clip);
    entry.clip = assetRelative(project, file);
    if (project) entry.guid = project->getAssetDatabase().getGuid(file);
    return entry;
}

std::string ClipConverters::uniqueClipPath(Project* project, const std::string& directory, const std::string& stem){
    // a file name: no separators or characters file systems refuse
    std::string name;
    for (char c : stem){
        name += (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
                 (unsigned char)c < 32) ? '_' : c;
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    if (name.empty()) name = "New Animation";
    std::string dir = directory;
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
    if (dir == ".") dir.clear();
    const std::string prefix = dir.empty() ? "" : dir + "/";
    if (!project) return prefix + name + ".anim";
    const fs::path base = project->getProjectPath();
    for (int k = 1; k < 10000; k++){
        const std::string candidate = prefix + name + (k == 1 ? "" : " " + std::to_string(k)) + ".anim";
        std::error_code ec;
        if (!fs::exists(base / editor::FileUtils::pathFromUtf8(candidate), ec)) return candidate;
    }
    return prefix + name + ".anim";
}

std::string ClipConverters::defaultClipDirectory(Project* project, uint32_t sceneId, Entity root){
    std::string assetsDir;
    if (project){
        assetsDir = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(project->getAssetsPath()));
        if (assetsDir == ".") assetsDir.clear();
    }
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene || root == NULL_ENTITY || !sceneProject->scene->isEntityCreated(root)){
        return assetsDir;
    }
    Scene* scene = sceneProject->scene;
    auto directoryOf = [&](const fs::path& absolute){
        std::string dir = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(absolute.parent_path()));
        if (dir == ".") dir.clear();
        // only inside the project
        return dir.compare(0, 2, "..") == 0 || editor::FileUtils::pathFromUtf8(dir).is_absolute() ? assetsDir : dir;
    };
    // the model on the root, else the first one under it
    Entity modelEntity = scene->findComponent<ModelComponent>(root) ? root : NULL_ENTITY;
    if (modelEntity == NULL_ENTITY){
        for (const auto& entry : anim::descendantPaths(scene, root)){
            if (scene->findComponent<ModelComponent>(entry.second)){
                modelEntity = entry.second;
                break;
            }
        }
    }
    if (modelEntity != NULL_ENTITY){
        const ModelComponent& model = scene->getComponent<ModelComponent>(modelEntity);
        if (!model.filename.empty()){
            return directoryOf(project->resolveAssetPath(FileUtils::pathFromUtf8(model.filename.path)));
        }
    }
    if (const PrefabComponent* prefab = scene->findComponent<PrefabComponent>(root)){
        if (!prefab->path.empty()){
            return directoryOf(project->getProjectPath() / editor::FileUtils::pathFromUtf8(prefab->path));
        }
    }
    return assetsDir;
}
