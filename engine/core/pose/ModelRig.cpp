// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/ModelRig.h"

#include "Scene.h"
#include "component/BoneComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "pose/PoseFile.h"

#include "tiny_gltf.h"

#include <cstring>
#include <map>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::pose;

namespace {

    Matrix4 trsMatrix(const Vector3& position, const Quaternion& rotation, const Vector3& scale){
        return Matrix4::translateMatrix(position) * rotation.getRotationMatrix() * Matrix4::scaleMatrix(scale);
    }

    // one element of a glTF accessor as floats (integers as their values, or normalised when
    // `normalized`); false when the accessor cannot be read
    bool readAccessor(const tinygltf::Model& gltf, int accessorIndex, size_t element, int components, float* out){
        if (accessorIndex < 0 || accessorIndex >= (int)gltf.accessors.size()) return false;
        const tinygltf::Accessor& accessor = gltf.accessors[accessorIndex];
        if (element >= accessor.count) return false;
        if (accessor.bufferView < 0 || accessor.bufferView >= (int)gltf.bufferViews.size()) return false;
        const tinygltf::BufferView& view = gltf.bufferViews[accessor.bufferView];
        if (view.buffer < 0 || view.buffer >= (int)gltf.buffers.size()) return false;
        const std::vector<unsigned char>& data = gltf.buffers[view.buffer].data;

        const int componentSize = tinygltf::GetComponentSizeInBytes(accessor.componentType);
        const int available = tinygltf::GetNumComponentsInType(accessor.type);
        if (componentSize <= 0 || available < components) return false;
        const int stride = accessor.ByteStride(view);
        if (stride <= 0) return false;

        const size_t offset = view.byteOffset + accessor.byteOffset + element * (size_t)stride;
        if (offset > data.size() || (size_t)(componentSize * components) > data.size() - offset) return false;
        const unsigned char* p = data.data() + offset;

        for (int c = 0; c < components; c++){
            const unsigned char* v = p + c * componentSize;
            float value = 0.0f;
            switch (accessor.componentType){
                case TINYGLTF_COMPONENT_TYPE_FLOAT: { float f; std::memcpy(&f, v, 4); value = f; break; }
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: { uint8_t u = *v; value = accessor.normalized ? u / 255.0f : (float)u; break; }
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: { uint16_t u; std::memcpy(&u, v, 2); value = accessor.normalized ? u / 65535.0f : (float)u; break; }
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: { uint32_t u; std::memcpy(&u, v, 4); value = (float)u; break; }
                case TINYGLTF_COMPONENT_TYPE_BYTE: { int8_t s; std::memcpy(&s, v, 1); value = accessor.normalized ? std::max(s / 127.0f, -1.0f) : (float)s; break; }
                case TINYGLTF_COMPONENT_TYPE_SHORT: { int16_t s; std::memcpy(&s, v, 2); value = accessor.normalized ? std::max(s / 32767.0f, -1.0f) : (float)s; break; }
                default: return false;
            }
            out[c] = value;
        }
        return true;
    }

    // WEIGHTS_0 stored as integers are normalised by definition (glTF 2.0)
    bool readWeights(const tinygltf::Model& gltf, int accessorIndex, size_t element, float* out){
        if (!readAccessor(gltf, accessorIndex, element, 4, out)) return false;
        const tinygltf::Accessor& accessor = gltf.accessors[accessorIndex];
        if (!accessor.normalized){
            if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE){
                for (int c = 0; c < 4; c++) out[c] /= 255.0f;
            }else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT){
                for (int c = 0; c < 4; c++) out[c] /= 65535.0f;
            }
        }
        return true;
    }

    bool sameSkeleton(const Skeleton& a, const Skeleton& b){
        if (a.bones.size() != b.bones.size()) return false;
        for (size_t i = 0; i < a.bones.size(); i++){
            const SkeletonBone& x = a.bones[i];
            const SkeletonBone& y = b.bones[i];
            if (x.name != y.name || x.parent != y.parent || x.position != y.position ||
                    x.rotation != y.rotation || x.scale != y.scale){
                return false;
            }
        }
        return true;
    }

}

bool pose::buildSkeleton(Scene* scene, Entity model, Skeleton& skeleton, std::vector<Entity>& boneEntities){
    return buildSkeleton(scene, model, skeleton, boneEntities, nullptr);
}

bool pose::buildSkeleton(Scene* scene, Entity model, Skeleton& skeleton, std::vector<Entity>& boneEntities,
                         std::vector<Matrix4>* bridges){
    skeleton.bones.clear();
    boneEntities.clear();
    if (bridges) bridges->clear();
    if (!scene || !scene->isEntityCreated(model)) return false;
    const ModelComponent* modelComponent = scene->findComponent<ModelComponent>(model);
    if (!modelComponent || modelComponent->bonesIdMapping.empty()) return false;

    // glTF node order: parents mostly before children, and the same order on every load
    std::unordered_map<Entity, int> indexOf;
    for (const auto& bone : modelComponent->bonesIdMapping){
        const Entity entity = bone.second;
        if (entity == NULL_ENTITY || !scene->isEntityCreated(entity) || indexOf.count(entity)) continue;
        if (!scene->findComponent<BoneComponent>(entity) || !scene->findComponent<Transform>(entity)) continue;
        indexOf[entity] = (int)boneEntities.size();
        boneEntities.push_back(entity);
    }
    if (boneEntities.empty()) return false;

    skeleton.bones.resize(boneEntities.size());
    if (bridges) bridges->assign(boneEntities.size(), Matrix4());

    for (size_t i = 0; i < boneEntities.size(); i++){
        const Entity entity = boneEntities[i];
        const BoneComponent& bone = scene->getComponent<BoneComponent>(entity);
        SkeletonBone& out = skeleton.bones[i];
        out.name = scene->getEntityName(entity);
        out.parent = -1;

        // non-bone nodes between the bone and its parent bone (or the model) are folded in
        Matrix4 bridge;
        bool hasBridge = false;
        Entity parent = scene->getComponent<Transform>(entity).parent;
        int guard = 0;
        while (parent != NULL_ENTITY && parent != model && guard++ < 1024){
            auto found = indexOf.find(parent);
            if (found != indexOf.end()){
                out.parent = found->second;
                break;
            }
            const Transform* transform = scene->findComponent<Transform>(parent);
            if (!transform) break;
            bridge = trsMatrix(transform->position, transform->rotation, transform->scale) * bridge;
            hasBridge = true;
            parent = transform->parent;
        }

        if (hasBridge){
            const Matrix4 local = bridge * trsMatrix(bone.bindPosition, bone.bindRotation, bone.bindScale);
            local.decompose(out.position, out.scale, out.rotation);
            if (bridges) (*bridges)[i] = bridge;
        }else{
            out.position = bone.bindPosition;
            out.rotation = bone.bindRotation;
            out.scale = bone.bindScale;
        }
    }
    return true;
}

std::shared_ptr<PoseSkin> pose::buildSkin(Scene* scene, Entity model, const Skeleton& skeleton,
                                          const std::vector<Entity>& boneEntities){
    if (!scene || !scene->isEntityCreated(model)) return nullptr;
    const ModelComponent* modelComponent = scene->findComponent<ModelComponent>(model);
    if (!modelComponent || !modelComponent->gltfModel) return nullptr;
    const tinygltf::Model& gltf = *modelComponent->gltfModel;

    std::unordered_map<Entity, int> indexOf;
    for (size_t i = 0; i < boneEntities.size(); i++) indexOf[boneEntities[i]] = (int)i;

    struct SkinnedPrimitive {
        int position, joints, weights;
        std::vector<int> jointBones;   // skin joint -> skeleton bone (-1 unknown)
        size_t count;
    };
    std::vector<SkinnedPrimitive> primitives;
    size_t total = 0;

    for (size_t n = 0; n < gltf.nodes.size(); n++){
        const tinygltf::Node& node = gltf.nodes[n];
        if (node.mesh < 0 || node.mesh >= (int)gltf.meshes.size() || node.skin < 0 || node.skin >= (int)gltf.skins.size()) continue;
        const tinygltf::Skin& skin = gltf.skins[node.skin];

        std::vector<int> jointBones(skin.joints.size(), -1);
        for (size_t j = 0; j < skin.joints.size(); j++){
            auto bone = modelComponent->bonesIdMapping.find(skin.joints[j]);
            if (bone == modelComponent->bonesIdMapping.end()) continue;
            auto index = indexOf.find(bone->second);
            if (index != indexOf.end()) jointBones[j] = index->second;
        }

        for (const tinygltf::Primitive& primitive : gltf.meshes[node.mesh].primitives){
            auto position = primitive.attributes.find("POSITION");
            auto joints = primitive.attributes.find("JOINTS_0");
            auto weights = primitive.attributes.find("WEIGHTS_0");
            if (position == primitive.attributes.end() || joints == primitive.attributes.end() || weights == primitive.attributes.end()) continue;
            if (position->second < 0 || position->second >= (int)gltf.accessors.size()) continue;
            const size_t count = gltf.accessors[position->second].count;
            if (count == 0) continue;
            primitives.push_back({position->second, joints->second, weights->second, jointBones, count});
            total += count;
        }
    }
    if (total == 0) return nullptr;

    auto out = std::make_shared<PoseSkin>();
    out->inverseBind.assign(skeleton.bones.size(), Matrix4());
    for (size_t i = 0; i < boneEntities.size() && i < skeleton.bones.size(); i++){
        if (const BoneComponent* bone = scene->findComponent<BoneComponent>(boneEntities[i])){
            out->inverseBind[i] = bone->offsetMatrix;
        }
    }

    // every stride-th vertex of all the skinned vertices together, starting at 0
    const size_t stride = std::max<size_t>(1, PoseSkin::sampleStride(total));
    size_t base = 0;
    for (const SkinnedPrimitive& primitive : primitives){
        size_t first = (base % stride == 0) ? 0 : stride - base % stride;
        for (size_t v = first; v < primitive.count && out->vertices.size() < PoseSkin::MAX_VERTICES; v += stride){
            float position[3], joints[4], weights[4];
            if (!readAccessor(gltf, primitive.position, v, 3, position) ||
                    !readAccessor(gltf, primitive.joints, v, 4, joints) ||
                    !readWeights(gltf, primitive.weights, v, weights)){
                continue;
            }
            PoseSkinVertex vertex;
            vertex.position = Vector3(position[0], position[1], position[2]);
            int used = 0;
            for (int k = 0; k < 4; k++){
                const int joint = (int)joints[k];
                if (weights[k] <= 0.0f || joint < 0 || joint >= (int)primitive.jointBones.size()) continue;
                const int bone = primitive.jointBones[joint];
                if (bone < 0) continue;
                vertex.bones[used] = bone;
                vertex.weights[used] = weights[k];
                used++;
            }
            if (used > 0) out->vertices.push_back(vertex);
        }
        base += primitive.count;
    }
    if (out->vertices.empty()) return nullptr;
    return out;
}

bool pose::resolveHumanoid(const Skeleton& skeleton, const HumanoidOverrides& overrides, HumanoidMap& out,
                           std::string* error, std::vector<std::string>* notes){
    out = autoDetectHumanoid(skeleton, notes);

    for (int s = 0; s < HUMAN_BONE_COUNT; s++){
        const std::optional<std::string>& value = overrides.bones[s];
        if (!value.has_value()) continue;
        const char* slot = humanBoneName(boneAt(s));
        if (value->empty()){
            out.bones[s].clear();
            if (notes) notes->push_back(std::string(slot) + ": unmapped by the sidecar");
            continue;
        }
        if (findSkeletonBone(skeleton, *value) < 0){
            if (error) *error = std::string("humanoid sidecar: ") + slot + ": the skeleton has no bone '" + *value + "'";
            return false;
        }
        // the bone leaves the slot auto-detect gave it, unless that slot is overridden too
        for (int t = 0; t < HUMAN_BONE_COUNT; t++){
            if (t != s && out.bones[t] == *value && !overrides.bones[t].has_value()){
                out.bones[t].clear();
                if (notes) notes->push_back(std::string(humanBoneName(boneAt(t))) + ": unmapped, its bone '" + *value + "' is forced to " + slot);
            }
        }
        out.bones[s] = *value;
        if (notes) notes->push_back(std::string(slot) + ": '" + *value + "' (sidecar)");
    }

    return validateHumanoid(skeleton, out, error);
}

std::string pose::humanoidSidecarStamp(const std::string& modelPath){
    std::string text;
    std::string error;
    bool notFound = false;
    if (!readTextFile(humanoidSidecarPath(modelPath), text, &error, &notFound)) return "";
    return text;
}

PoseRigCache& PoseRigCache::shared(){
    static PoseRigCache cache;
    return cache;
}

bool PoseRigCache::get(const std::string& modelPath, const std::string& sidecarStamp, const Skeleton& skeleton,
                       const HumanoidOverrides& overrides, const std::function<std::shared_ptr<PoseSkin>()>& skinFactory,
                       ModelRigInfo& out, std::string* error){
    std::lock_guard<std::mutex> lock(mutex);
    for (const Entry& entry : entries){
        if (entry.modelPath == modelPath && entry.stamp == sidecarStamp && sameSkeleton(entry.skeleton, skeleton)){
            out = entry.info;
            return true;
        }
    }

    ModelRigInfo info;
    if (!resolveHumanoid(skeleton, overrides, info.humanoid, error, &info.notes)) return false;
    info.skin = skinFactory ? skinFactory() : nullptr;
    info.rig = std::make_shared<PoseRig>();
    // the skin first: it sets the floor and the sole offsets the neutral pose is grounded with
    if (info.skin) info.rig->setSkin(info.skin);
    if (!info.rig->build(skeleton, info.humanoid, error)) return false;

    // one rig per model file: an older sidecar or skeleton is replaced
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& entry){ return entry.modelPath == modelPath; }), entries.end());
    entries.push_back({modelPath, sidecarStamp, skeleton, info});
    out = info;
    return true;
}

void PoseRigCache::remove(const std::string& modelPath){
    std::lock_guard<std::mutex> lock(mutex);
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& entry){ return entry.modelPath == modelPath; }), entries.end());
}

void PoseRigCache::clear(){
    std::lock_guard<std::mutex> lock(mutex);
    entries.clear();
}

size_t PoseRigCache::size() const{
    std::lock_guard<std::mutex> lock(mutex);
    return entries.size();
}

bool pose::buildModelRig(Scene* scene, Entity model, ModelRigInfo& out, std::vector<Entity>& boneEntities,
                         std::vector<Matrix4>* bridges, std::string* error){
    const ModelComponent* modelComponent = scene ? scene->findComponent<ModelComponent>(model) : nullptr;
    if (!modelComponent){
        if (error) *error = "the entity has no Model";
        return false;
    }
    Skeleton skeleton;
    if (!buildSkeleton(scene, model, skeleton, boneEntities, bridges)){
        if (error) *error = "the model '" + modelComponent->filename + "' has no skeleton";
        return false;
    }

    // the sidecar next to the model file; missing = all auto-detect
    const std::string stamp = humanoidSidecarStamp(modelComponent->filename);
    HumanoidOverrides overrides;
    std::vector<std::string> warnings;
    if (!stamp.empty()){
        std::string parseError;
        if (!parseHumanoidOverrides(stamp, overrides, warnings, &parseError)){
            if (error) *error = humanoidSidecarPath(modelComponent->filename) + ": " + parseError;
            return false;
        }
    }

    const std::string modelPath = modelComponent->filename;
    if (!PoseRigCache::shared().get(modelPath, stamp, skeleton, overrides,
            [&]() { return buildSkin(scene, model, skeleton, boneEntities); }, out, error)){
        if (error && !error->empty()) *error = modelPath + ": " + *error;
        return false;
    }
    for (const std::string& warning : warnings){
        out.notes.push_back(humanoidSidecarPath(modelPath) + ": " + warning);
    }
    return true;
}
