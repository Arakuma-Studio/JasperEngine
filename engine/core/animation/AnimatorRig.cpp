// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimatorRig.h"

#include "Scene.h"
#include "animation/ClipBinding.h"
#include "component/LatticeComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "lattice/LatticeSolver.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <unordered_set>

using namespace jasperengine;
using namespace jasperengine::anim;

namespace{

    uint64_t channelKey(Entity entity, ClipChannel kind, int index){
        return ((uint64_t)entity << 32) ^ ((uint64_t)(uint8_t)kind << 28) ^ (uint64_t)(uint32_t)index;
    }

    // `entity` is `root` or under it (by Transform parents)
    bool isUnder(Scene* scene, Entity root, Entity entity){
        Entity current = entity;
        int guard = 0;
        while (current != NULL_ENTITY && guard++ < 4096){
            if (current == root) return true;
            Transform* transform = scene->findComponent<Transform>(current);
            if (!transform) return false;
            current = transform->parent;
        }
        return false;
    }

    RigRest& restOf(Scene* scene, Entity entity, AnimatorRestMap& rest){
        RigRest& value = rest[entity];
        if (!value.hasTransform){
            if (Transform* transform = scene->findComponent<Transform>(entity)){
                value.hasTransform = true;
                value.position = transform->position;
                value.rotation = transform->rotation;
                value.scale = transform->scale;
            }
        }
        return value;
    }

    void collectModels(Scene* scene, Entity entity, std::vector<Entity>& out, int depth){
        if (depth > 256) return;
        if (scene->findComponent<ModelComponent>(entity)) out.push_back(entity);
        for (Entity child : getChildren(scene, entity)) collectModels(scene, child, out, depth + 1);
    }

}

int anim::AnimatorRig::findBone(const std::string& path) const{
    auto it = byPath.find(path);
    return it == byPath.end() ? -1 : it->second;
}

int anim::AnimatorRig::findBone(Entity entity) const{
    auto it = byEntity.find(entity);
    return it == byEntity.end() ? -1 : it->second;
}

bool anim::AnimatorRig::stillValid(Scene* scene) const{
    for (size_t i = 0; i < bones.size(); i++){
        const RigBone& bone = bones[i];
        if (!scene->isEntityCreated(bone.entity)) return false;
        Transform* transform = scene->findComponent<Transform>(bone.entity);
        if (!transform || transform->parent != bone.parentEntity) return false;
        if (!(rootBone && i == 0) && scene->getEntityNameRef(bone.entity) != bone.name) return false;
    }
    for (const RigChannel& channel : channels){
        if (!scene->isEntityCreated(channel.entity)) return false;
    }
    for (const RigSkin& skin : skins){
        if (!scene->isEntityCreated(skin.mesh) || !scene->findComponent<ModelComponent>(skin.model)) return false;
    }
    return true;
}

Entity anim::AnimatorRig::channelEntity(const std::string& path) const{
    auto it = channelPathEntities.find(path);
    return it == channelPathEntities.end() ? NULL_ENTITY : it->second;
}

int anim::AnimatorRig::morphWidth(Entity entity) const{
    auto it = morphWidths.find(entity);
    return it == morphWidths.end() ? 0 : it->second;
}

int anim::AnimatorRig::findProperty(Entity entity, uint32_t key) const{
    auto it = propertySlots.find(((uint64_t)entity << 32) | key);
    return it == propertySlots.end() ? -1 : it->second;
}

int anim::AnimatorRig::findChannel(Entity entity, ClipChannel kind, int index) const{
    auto it = channelSlots.find(channelKey(entity, kind, index));
    return it == channelSlots.end() ? -1 : it->second;
}

void anim::AnimatorRig::build(Scene* scene, Entity root, const std::vector<std::string>& paths,
                              const std::vector<std::pair<std::string, std::pair<ClipChannel, int>>>& channelPaths, AnimatorRestMap& restMap,
                              const std::vector<RigPropertyRequest>& propertyPaths){
    bones.clear();
    parents.clear();
    channels.clear();
    skins.clear();
    properties.clear();
    propertySlots.clear();
    byPath.clear();
    byEntity.clear();
    channelSlots.clear();
    channelPathEntities.clear();
    morphWidths.clear();
    rootBone = false;

    // the entities the rig holds: every resolved path and its ancestors below the root
    std::unordered_set<Entity> needed;
    auto need = [&](Entity entity){
        Entity current = entity;
        int guard = 0;
        while (current != NULL_ENTITY && current != root && guard++ < 4096){
            if (!needed.insert(current).second) break;
            Transform* transform = scene->findComponent<Transform>(current);
            if (!transform) break;
            current = transform->parent;
        }
    };
    unresolved = 0;
    for (const std::string& path : paths){
        if (path.empty()){
            rootBone = scene->findComponent<Transform>(root) != nullptr;
            continue;
        }
        const Entity entity = resolvePath(scene, root, path);
        if (entity != NULL_ENTITY && scene->findComponent<Transform>(entity)) need(entity);
        else unresolved++;
    }

    // skins of the models on or under the root whose joints all are under it
    struct PendingSkin{
        Entity model;
        size_t binding;
    };
    std::vector<PendingSkin> pendingSkins;
    std::vector<Entity> models;
    collectModels(scene, root, models, 0);
    for (Entity modelEntity : models){
        ModelComponent& model = scene->getComponent<ModelComponent>(modelEntity);
        for (size_t b = 0; b < model.skinBindings.size(); b++){
            const ModelSkinBinding& binding = model.skinBindings[b];
            if (binding.mesh == NULL_ENTITY || binding.joints.empty() || !scene->findComponent<MeshComponent>(binding.mesh)) continue;
            bool allUnder = isUnder(scene, root, binding.mesh);
            for (size_t j = 0; j < binding.joints.size() && allUnder; j++){
                allUnder = binding.joints[j] != root && scene->findComponent<Transform>(binding.joints[j]) &&
                           isUnder(scene, root, binding.joints[j]);
            }
            if (!allUnder) continue;
            for (Entity joint : binding.joints) need(joint);
            pendingSkins.push_back({modelEntity, b});
        }
    }

    // parents first: the hierarchy walked from the root in child order
    if (rootBone){
        RigBone bone;
        bone.entity = root;
        bone.parent = -1;
        bone.name = scene->getEntityName(root);
        bones.push_back(bone);
        byEntity[root] = 0;
        byPath[""] = 0;
    }
    std::vector<std::pair<Entity, int>> stack;
    {
        std::vector<Entity> children = getChildren(scene, root);
        for (auto it = children.rbegin(); it != children.rend(); ++it) stack.push_back({*it, -1});
    }
    while (!stack.empty()){
        const auto [entity, parent] = stack.back();
        stack.pop_back();
        if (!needed.count(entity)) continue;
        const int index = (int)bones.size();
        RigBone bone;
        bone.entity = entity;
        bone.parent = parent;
        bone.name = scene->getEntityName(entity);
        if (relativePath(scene, root, entity, bone.path)) byPath[bone.path] = index;
        else bone.path.clear();
        bones.push_back(bone);
        byEntity[entity] = index;
        std::vector<Entity> children = getChildren(scene, entity);
        for (auto it = children.rbegin(); it != children.rend(); ++it) stack.push_back({*it, index});
    }
    parents.resize(bones.size());
    for (size_t i = 0; i < bones.size(); i++){
        parents[i] = bones[i].parent;
        Transform* transform = scene->findComponent<Transform>(bones[i].entity);
        bones[i].parentEntity = transform ? transform->parent : NULL_ENTITY;
    }

    // channels: morph weights 0..widest of an entity contiguous, lattice points 3 floats each
    std::map<Entity, int> widths;
    std::map<Entity, std::set<int>> latticePoints;
    for (const auto& request : channelPaths){
        const Entity entity = resolvePath(scene, root, request.first);
        if (entity == NULL_ENTITY) continue;
        if (request.second.first == ClipChannel::Morph){
            if (!scene->findComponent<MeshComponent>(entity)) continue;
            channelPathEntities[request.first] = entity;
            int& width = widths[entity];
            width = std::max(width, std::min(request.second.second, (int)MAX_MORPHTARGETS));
        }else if (request.second.first == ClipChannel::Lattice){
            LatticeComponent* lattice = scene->findComponent<LatticeComponent>(entity);
            if (!lattice) continue;
            const int count = lattice::pointCount(lattice->resolutionX, lattice->resolutionY, lattice->resolutionZ);
            channelPathEntities[request.first] = entity;
            if (request.second.second >= 0 && request.second.second < count) latticePoints[entity].insert(request.second.second);
        }
    }
    for (const auto& entry : widths){
        morphWidths[entry.first] = entry.second;
        for (int k = 0; k < entry.second; k++){
            channelSlots[channelKey(entry.first, ClipChannel::Morph, k)] = (int)channels.size();
            channels.push_back({entry.first, ClipChannel::Morph, k, 0});
        }
    }
    for (const auto& entry : latticePoints){
        for (int point : entry.second){
            channelSlots[channelKey(entry.first, ClipChannel::Lattice, point)] = (int)channels.size();
            for (int axis = 0; axis < 3; axis++) channels.push_back({entry.first, ClipChannel::Lattice, point, axis});
        }
    }
    // properties: whole-property tracks first (a property without a declared width takes the
    // width of its whole track, not a swizzle track's 1)
    for (int pass = 0; pass < 2; pass++){
        for (const RigPropertyRequest& request : propertyPaths){
            PropertyRef ref;
            if (!parsePropertyRef(request.component, request.property, ref)) continue;
            if ((ref.swizzle >= 0) != (pass == 1)) continue;
            const Entity entity = request.path.empty() ? root : resolvePath(scene, root, request.path);
            if (entity == NULL_ENTITY) continue;
            PropertyBinding binding = bindProperty(scene, entity, ref, ref.swizzle >= 0 ? 1 : request.keyWidth);
            if (binding.status != PropertyBindStatus::Bound || binding.key == 0 || binding.width == 0) continue;
            channelPathEntities[request.path] = entity;
            const uint64_t slot = ((uint64_t)entity << 32) | binding.key;
            if (propertySlots.count(slot)) continue;
            binding.swizzle = -1;
            RigProperty property;
            property.entity = entity;
            property.binding = binding;
            property.firstChannel = (int)channels.size();
            propertySlots[slot] = (int)properties.size();
            for (int k = 0; k < (int)binding.width; k++) channels.push_back({entity, ClipChannel::Property, (int)properties.size(), k});
            properties.push_back(std::move(property));
        }
    }

    // rest values
    rest.resize(bones.size(), channels.size());
    for (size_t i = 0; i < bones.size(); i++){
        const RigRest& value = restOf(scene, bones[i].entity, restMap);
        rest.setPosition(i, value.position);
        rest.setRotation(i, value.rotation);
        rest.setScale(i, value.scale);
    }
    float* channelRest = rest.channelData();
    for (size_t c = 0; c < channels.size(); c++){
        const RigChannel& channel = channels[c];
        RigRest& value = restMap[channel.entity];
        if (channel.kind == ClipChannel::Morph){
            if (value.morph.empty()){
                if (MeshComponent* mesh = scene->findComponent<MeshComponent>(channel.entity)){
                    value.morph.assign(mesh->morphWeights, mesh->morphWeights + MAX_MORPHTARGETS);
                }else{
                    value.morph.assign(MAX_MORPHTARGETS, 0.0f);
                }
            }
            channelRest[c] = channel.index < (int)value.morph.size() ? value.morph[channel.index] : 0.0f;
        }else if (channel.kind == ClipChannel::Property){
            const RigProperty& property = properties[(size_t)channel.index];
            RigPropertyRest& propertyRest = value.properties[property.binding.key];
            if (propertyRest.values.size() != property.binding.width){
                propertyRest.binding = property.binding;
                propertyRest.values.assign(property.binding.width, 0.0f);
                readPropertyKeySpace(scene, channel.entity, property.binding, propertyRest.values.data());
            }
            channelRest[c] = propertyRest.values[(size_t)channel.axis];
        }else{
            if (value.lattice.empty()){
                if (LatticeComponent* lattice = scene->findComponent<LatticeComponent>(channel.entity)){
                    const int count = lattice::pointCount(lattice->resolutionX, lattice->resolutionY, lattice->resolutionZ);
                    value.lattice.assign((size_t)count, Vector3(0, 0, 0));
                    for (size_t p = 0; p < value.lattice.size() && p < lattice->offsets.size(); p++) value.lattice[p] = lattice->offsets[p];
                }
            }
            const Vector3 offset = channel.index < (int)value.lattice.size() ? value.lattice[channel.index] : Vector3(0, 0, 0);
            channelRest[c] = channel.axis == 0 ? offset.x : (channel.axis == 1 ? offset.y : offset.z);
        }
    }

    // skins: joints as bones, the mesh node's chain up to its nearest rig ancestor
    for (const PendingSkin& pending : pendingSkins){
        const ModelSkinBinding& binding = scene->getComponent<ModelComponent>(pending.model).skinBindings[pending.binding];
        RigSkin skin;
        skin.model = pending.model;
        skin.binding = pending.binding;
        skin.mesh = binding.mesh;
        skin.meshBone = findBone(binding.mesh);
        bool valid = true;
        for (Entity joint : binding.joints){
            const int bone = findBone(joint);
            if (bone < 0 || (rootBone && bone == 0)){
                valid = false;
                break;
            }
            skin.joints.push_back(bone);
        }
        if (!valid) continue;
        skin.inverseBindMatrices = binding.inverseBindMatrices;
        if (skin.meshBone < 0){
            Entity current = binding.mesh;
            int guard = 0;
            while (current != NULL_ENTITY && current != root && findBone(current) < 0 && guard++ < 4096){
                skin.meshChain.push_back(current);
                Transform* transform = scene->findComponent<Transform>(current);
                current = transform ? transform->parent : NULL_ENTITY;
            }
            skin.meshParentBone = (current != NULL_ENTITY && current != root) ? findBone(current) : -1;
        }
        skins.push_back(std::move(skin));
    }
}

// ---------------------------------------------------------------- binding and sampling

void anim::RigClipBinding::bind(const std::shared_ptr<const AnimationClip>& newClip, const AnimatorRig& rig, const std::vector<uint8_t>* include){
    clip = newClip;
    targets.clear();
    groups.clear();
    latticeSlots.clear();
    boundCount = 0;
    trackCount = 0;
    if (!clip) return;
    const std::vector<AnimationClip::Track>& tracks = clip->getTracks();
    for (size_t t = 0; t < tracks.size(); t++){
        const AnimationClip::Track& track = tracks[t];
        if (!track.isSampled() || !track.times || isHumanoidChannel(track.channel)) continue;
        if (include && (t >= include->size() || !(*include)[t])) continue;
        trackCount++;
        Target target;
        target.channel = track.channel;
        target.relative = track.relative;
        target.track = t;
        if (track.channel == ClipChannel::Position || track.channel == ClipChannel::Rotation || track.channel == ClipChannel::Scale){
            target.index = rig.findBone(track.path);
            // the root bone is written as the root's own transform (never part of rig space)
        }else if (track.channel == ClipChannel::Morph){
            const Entity entity = rig.channelEntity(track.path);
            target.index = entity != NULL_ENTITY ? rig.findChannel(entity, ClipChannel::Morph, 0) : -1;
            target.width = (int)std::min(track.width, (size_t)rig.morphWidth(entity));
        }else if (track.channel == ClipChannel::Lattice){
            const Entity entity = rig.channelEntity(track.path);
            if (entity != NULL_ENTITY && track.points){
                target.index = (int)latticeSlots.size();
                for (size_t n = 0; n < track.width; n++){
                    latticeSlots.push_back(rig.findChannel(entity, ClipChannel::Lattice, track.points[n]));
                }
                target.width = (int)track.width;
            }
        }else if (track.channel == ClipChannel::Property){
            const Entity entity = rig.channelEntity(track.path);
            PropertyRef ref;
            if (entity != NULL_ENTITY && parsePropertyRef(track.component, track.property, ref)){
                const int slot = rig.findProperty(entity, internPropertyKey(track.component, ref));
                if (slot >= 0){
                    const RigProperty& property = rig.properties[(size_t)slot];
                    if (ref.swizzle >= 0){
                        if (ref.swizzle < (int)property.binding.width){
                            target.index = property.firstChannel + ref.swizzle;
                            target.width = 1;
                        }
                    }else{
                        target.index = property.firstChannel;
                        target.width = (int)std::min(track.width, (size_t)property.binding.width);
                        target.overridden = track.overridden;
                        if (keysOpaqueAlpha(property.binding, track.width)) target.alpha = property.firstChannel + 3;
                    }
                    target.stepped = property.binding.discrete && !track.discrete;
                }
            }
        }
        // key-time groups: the same times, easings and loop
        int group = -1;
        for (size_t g = 0; g < groups.size(); g++){
            const AnimationClip::Track& other = tracks[groups[g].track];
            if (other.keyCount != track.keyCount || other.loop != track.loop || other.duration != track.duration) continue;
            // tracks with the same times usually share one range (AnimationClip::Track::times)
            if (other.times != track.times && std::memcmp(other.times, track.times, track.keyCount * sizeof(float)) != 0) continue;
            if ((other.easings == nullptr) != (track.easings == nullptr)) continue;
            if (track.easings && std::memcmp(other.easings, track.easings, track.keyCount * sizeof(EaseType)) != 0) continue;
            group = (int)g;
            break;
        }
        if (group < 0){
            group = (int)groups.size();
            groups.push_back({t});
        }
        target.group = group;
        if (target.index >= 0) boundCount++;
        targets.push_back(target);
    }
}

void anim::RigClipCursors::resize(size_t groups){
    cursors.assign(groups, KeyCursor());
    locations.assign(groups, Location());
}

void anim::sampleClip(const RigClipBinding& binding, const AnimatorRig& rig, float clipTime, RigClipCursors& cursors, Pose& out,
                      float stepFps, int smoothBone){
    if (!binding.clip) return;
    const std::vector<AnimationClip::Track>& tracks = binding.clip->getTracks();
    if (cursors.cursors.size() != binding.groups.size()) cursors.resize(binding.groups.size());
    const bool held = stepFps > 0.0f && std::isfinite(stepFps);
    const bool smooth = held && smoothBone >= 0;
    if (smooth && cursors.smoothLocations.size() != binding.groups.size()) cursors.smoothLocations.resize(binding.groups.size());

    // one key search per group (sampleTrack's rule: exactly the first key at or before it, the
    // last key at or after it)
    auto locate = [](const AnimationClip::Track& track, float time, KeyCursor& cursor, RigClipCursors::Location& location){
        const size_t last = track.keyCount - 1;
        location.base = 0;
        location.at = KeyLocation();
        if (std::isnan(time) || time <= track.times[0]){
            cursor.index = 0;
        }else if (time >= track.times[last]){
            location.base = last;
            cursor.index = last;
        }else{
            location.at = locateKey(track.times, track.keyCount, track.easings, track.keyCount, time, &cursor);
        }
    };
    for (size_t g = 0; g < binding.groups.size(); g++){
        const AnimationClip::Track& track = tracks[binding.groups[g].track];
        const float time = trackTime(track, clipTime);
        locate(track, held ? holdPoseTime(track.times, track.keyCount, time, stepFps) : time, cursors.cursors[g], cursors.locations[g]);
        if (smooth){
            // (the cursor is only a search hint: a local one keeps the held search's)
            KeyCursor cursor = cursors.cursors[g];
            locate(track, time, cursor, cursors.smoothLocations[g]);
        }
    }

    const Pose& rest = rig.rest;
    float* tx = out.array(Pose::TX); float* ty = out.array(Pose::TY); float* tz = out.array(Pose::TZ);
    float* qx = out.array(Pose::QX); float* qy = out.array(Pose::QY); float* qz = out.array(Pose::QZ); float* qw = out.array(Pose::QW);
    float* sx = out.array(Pose::SX); float* sy = out.array(Pose::SY); float* sz = out.array(Pose::SZ);
    float* channels = out.channelData();
    for (const RigClipBinding::Target& target : binding.targets){
        if (target.index < 0) continue;
        const AnimationClip::Track& track = tracks[target.track];
        // the root bone's translation stays smooth under a Pose Frame Rate (S48 R23)
        const RigClipCursors::Location& location = (smooth && target.index == smoothBone && target.channel == ClipChannel::Position)
            ? cursors.smoothLocations[(size_t)target.group] : cursors.locations[(size_t)target.group];
        const KeyLocation& at = location.at;
        const size_t base = location.base;
        const bool cubic = at.index > 0 && track.hasTangents();
        const size_t i = (size_t)target.index;
        switch (target.channel){
            case ClipChannel::Position: {
                Vector3 v = sampleVector3(at, track.vectors + base, cubic ? track.inVectors + base : nullptr,
                                          cubic ? track.outVectors + base : nullptr);
                if (target.relative) v = rest.position(i) + v;
                tx[i] = v.x; ty[i] = v.y; tz[i] = v.z;
                break;
            }
            case ClipChannel::Scale: {
                Vector3 v = sampleVector3(at, track.vectors + base, cubic ? track.inVectors + base : nullptr,
                                          cubic ? track.outVectors + base : nullptr);
                if (target.relative) v = rest.scale(i) * v;
                sx[i] = v.x; sy[i] = v.y; sz[i] = v.z;
                break;
            }
            case ClipChannel::Rotation: {
                Quaternion q = sampleQuaternion(at, track.rotations + base, cubic ? track.inRotations + base : nullptr,
                                                cubic ? track.outRotations + base : nullptr);
                if (target.relative) q = rest.rotation(i) * q;
                qx[i] = q.x; qy[i] = q.y; qz[i] = q.z; qw[i] = q.w;
                break;
            }
            case ClipChannel::Morph: {
                const size_t width = track.width;
                const size_t previous = base + (at.index > 0 ? at.index - 1 : 0);
                const size_t next = base + at.index;
                sampleFloatRow(at, track.weights + previous * width, track.weights + next * width, (size_t)target.width,
                               cubic ? track.outWeights + previous * width : nullptr,
                               cubic ? track.inWeights + next * width : nullptr, channels + i);
                break;
            }
            case ClipChannel::Lattice: {
                const size_t width = track.width;
                const size_t previous = base + (at.index > 0 ? at.index - 1 : 0);
                const size_t next = base + at.index;
                for (int n = 0; n < target.width; n++){
                    const int slot = binding.latticeSlots[i + (size_t)n];
                    if (slot < 0) continue;
                    Vector3 v;
                    sampleVector3Row(at, track.vectors + previous * width + n, track.vectors + next * width + n, 1,
                                     cubic ? track.outVectors + previous * width + n : nullptr,
                                     cubic ? track.inVectors + next * width + n : nullptr, &v);
                    channels[slot] = v.x;
                    channels[slot + 1] = v.y;
                    channels[slot + 2] = v.z;
                }
                break;
            }
            case ClipChannel::Property: {
                // key space, as the player samples it; a discrete binding of interpolating keys steps
                const size_t width = track.width;
                const size_t previous = base + (at.index > 0 ? at.index - 1 : 0);
                const size_t next = base + at.index;
                const size_t count = std::min((size_t)target.width, std::min(width, (size_t)4));
                float values[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                if (target.stepped){
                    const size_t row = (at.index > 0 && at.interpolation < 1.0f) ? previous : next;
                    for (size_t n = 0; n < count; n++) values[n] = track.weights[row * width + n];
                }else{
                    sampleFloatRow(at, track.weights + previous * width, track.weights + next * width, count,
                                   cubic ? track.outWeights + previous * width : nullptr,
                                   cubic ? track.inWeights + next * width : nullptr, values);
                }
                for (size_t n = 0; n < count; n++){
                    if (target.overridden & (1u << n)) continue;
                    channels[i + n] = values[n];
                }
                if (target.alpha >= 0 && !(target.overridden & 8u)) channels[(size_t)target.alpha] = 1.0f;
                break;
            }
            // humanoid tracks play through the one retarget path (AnimatorRetarget), never by path
            case ClipChannel::Humanoid:
            case ClipChannel::HumanoidPosition:
                break;
        }
    }
}
