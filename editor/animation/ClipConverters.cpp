// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipConverters.h"

#include "Project.h"
#include "Scene.h"
#include "animation/AnimationClipIO.h"
#include "animation/ClipBinding.h"
#include "animation/ModelSubClips.h"
#include "component/ActionComponent.h"
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
        return "'" + scene->getEntityName(entity) + "'";
    }

    bool sameExtension(const std::string& path, const char* extension){
        std::string ext = FileData::getFilePathExtension(path);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return ext == extension;
    }

    // a model in the legacy layout (no node hierarchy): what MeshSystem::needsNodeHierarchyReload
    // checks, without the player condition
    bool isLegacyLayoutModel(Scene* scene, Entity entity){
        const ModelComponent* model = scene->findComponent<ModelComponent>(entity);
        if (!model || model->filename.empty() || model->loadedFilename.empty() || !model->nodesIdMapping.empty()) return false;
        if (sameExtension(model->filename, "obj")) return false;
        if (model->mergeStaticMeshes){
            size_t meshNodes = 0;
            if (model->gltfModel){
                for (const tinygltf::Node& node : model->gltfModel->nodes){
                    if (node.mesh >= 0) meshNodes++;
                }
            }
            if (!model->gltfModel || meshNodes > 1) return false;
        }
        return true;
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
        return ease == EaseType::CUSTOM ? EaseType::LINEAR : ease;
    }

    std::string clipNumber(float value){
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%g", value);
        return buffer;
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

    // the absolute file of a clip or model path as callers give it (absolute, project-relative
    // or asset-relative)
    fs::path absoluteFile(Project* project, const std::string& path){
        const fs::path given = fs::u8path(path);
        if (given.is_absolute() || !project) return given.lexically_normal();
        std::error_code ec;
        const fs::path inProject = (project->getProjectPath() / given).lexically_normal();
        if (fs::exists(inProject, ec)) return inProject;
        const fs::path inAssets = project->resolveAssetPath(given);
        if (fs::exists(inAssets, ec)) return inAssets;
        return inProject;
    }

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
        const ActionComponent* action = scene->findComponent<ActionComponent>(animation);
        root = action ? action->target : NULL_ENTITY;
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
        const Entity entity = frame.action;
        if (entity == NULL_ENTITY || !scene->isEntityCreated(entity)) continue;
        const ActionComponent* action = scene->findComponent<ActionComponent>(entity);
        const float speed = action ? action->speed : 1.0f;
        if (actions && speed > 0.0f){
            actionsEnd = std::max(actionsEnd, frame.startTime + actions->getFrameDuration(frame) / speed);
        }

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
        // before its frame starts the entity track leaves the target as it is: a step key at 0
        // holds that value (relative tracks: no change) until the first key
        for (size_t t = before; t < out.tracks.size(); t++){
            ClipTrackData& track = out.tracks[t];
            if (track.keys.empty() || track.keys.front().time <= anim::ANIM_TIME_EPSILON * 10.0f) continue;
            if (track.keys.front().hasTangents()){
                warn(report.warnings, "", label + ": starts at " + clipNumber(track.keys.front().time) +
                     " s; a cubic track holds its first key from 0 in the clip");
                continue;
            }
            ClipKey hold;
            hold.time = 0.0f;
            hold.ease = EaseType::STEP;
            if (track.relative){
                if (track.channel == ClipChannel::Position) hold.value = {0.0f, 0.0f, 0.0f};
                else if (track.channel == ClipChannel::Rotation) hold.value = {0.0f, 0.0f, 0.0f, 1.0f};
                else hold.value = {1.0f, 1.0f, 1.0f};
            }else if (!anim::readChannelValue(scene, target, track.channel, track.points, hold.value)){
                continue;
            }
            if (track.channel == ClipChannel::Morph) hold.value.resize(track.keys.front().value.size(), 0.0f);
            track.keys.insert(track.keys.begin(), hold);
        }
        if (out.tracks.size() > before){
            report.convertedTracks += out.tracks.size() - before;
            report.convertedTrackEntities.push_back(entity);
        }else{
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
    // a new asset (the model is not touched)
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
            key.ease = channel.ease == EaseType::CUSTOM ? EaseType::LINEAR : channel.ease;
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
        if (!fs::exists(base / fs::u8path(candidate), ec)) return candidate;
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
        return dir.compare(0, 2, "..") == 0 || fs::u8path(dir).is_absolute() ? assetsDir : dir;
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
            return directoryOf(project->resolveAssetPath(fs::u8path(model.filename)));
        }
    }
    if (const PrefabComponent* prefab = scene->findComponent<PrefabComponent>(root)){
        if (!prefab->path.empty()){
            return directoryOf(project->getProjectPath() / fs::u8path(prefab->path));
        }
    }
    return assetsDir;
}
