// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/ModelRig.h"

#include "Scene.h"
#include "component/BoneComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "animation/ModelSubClips.h"
#include "pose/PoseFile.h"

#include "tiny_gltf.h"

#include <algorithm>
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
        // the sparse substitution is not applied: refused rather than read wrong
        if (accessor.sparse.isSparse) return false;
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

    // MeshSystem::getGLTFNodeMatrix of a node (its matrix, else T * R * S)
    Matrix4 gltfNodeMatrix(const tinygltf::Model& gltf, int nodeIndex){
        if (nodeIndex < 0 || nodeIndex >= (int)gltf.nodes.size()) return Matrix4();
        const tinygltf::Node& node = gltf.nodes[nodeIndex];
        if (node.matrix.size() == 16){
            const std::vector<double>& m = node.matrix;
            return Matrix4(m[0], m[4], m[8], m[12],
                           m[1], m[5], m[9], m[13],
                           m[2], m[6], m[10], m[14],
                           m[3], m[7], m[11], m[15]);
        }
        Vector3 translation(0.0, 0.0, 0.0);
        Vector3 scale(1.0, 1.0, 1.0);
        Quaternion rotation(1.0, 0.0, 0.0, 0.0);
        if (node.translation.size() == 3) translation = Vector3(node.translation[0], node.translation[1], node.translation[2]);
        if (node.rotation.size() == 4) rotation = Quaternion(node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]);
        if (node.scale.size() == 3) scale = Vector3(node.scale[0], node.scale[1], node.scale[2]);
        return Matrix4::translateMatrix(translation) * rotation.getRotationMatrix() * Matrix4::scaleMatrix(scale);
    }

    // MeshSystem::getGLTFInverseBindMatrix: identity when it cannot be read
    Matrix4 gltfInverseBind(const tinygltf::Model& gltf, int skinIndex, size_t jointIndex){
        Matrix4 matrix;
        if (skinIndex < 0 || skinIndex >= (int)gltf.skins.size()) return matrix;
        const tinygltf::Skin& skin = gltf.skins[skinIndex];
        if (skin.inverseBindMatrices < 0 || skin.inverseBindMatrices >= (int)gltf.accessors.size()) return matrix;
        const tinygltf::Accessor& accessor = gltf.accessors[skin.inverseBindMatrices];
        if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT || accessor.type != TINYGLTF_TYPE_MAT4 ||
                jointIndex >= accessor.count || accessor.bufferView < 0 || accessor.bufferView >= (int)gltf.bufferViews.size()){
            return matrix;
        }
        const tinygltf::BufferView& view = gltf.bufferViews[accessor.bufferView];
        if (view.buffer < 0 || view.buffer >= (int)gltf.buffers.size()) return matrix;
        const std::vector<unsigned char>& data = gltf.buffers[view.buffer].data;
        const int byteStride = accessor.ByteStride(view);
        if (byteStride < (int)(sizeof(float) * 16)) return matrix;
        const size_t offset = view.byteOffset + accessor.byteOffset + jointIndex * byteStride;
        if (offset > data.size() || sizeof(float) * 16 > data.size() - offset) return matrix;
        float values[16];
        std::memcpy(values, data.data() + offset, sizeof(values));
        return Matrix4(values[0], values[4], values[8], values[12],
                       values[1], values[5], values[9], values[13],
                       values[2], values[6], values[10], values[14],
                       values[3], values[7], values[11], values[15]);
    }

    // The skin sample of buildSkin over glTF data: `jointBone(skin, joint)` is the skeleton bone of a
    // skin joint (-1 unknown), `inverseBind` per skeleton bone
    std::shared_ptr<PoseSkin> sampleSkin(const tinygltf::Model& gltf, const std::function<int(int, size_t)>& jointBone,
                                         const std::vector<Matrix4>& inverseBind){
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
                jointBones[j] = jointBone(node.skin, j);
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
        out->inverseBind = inverseBind;

        // every stride-th vertex of all the skinned vertices together, starting at 0
        const size_t stride = std::max<size_t>(1, PoseSkin::sampleStride(total));
        size_t base = 0;
        for (const SkinnedPrimitive& primitive : primitives){
            size_t first = (base % stride == 0) ? 0 : stride - base % stride;
            for (size_t v = first; v < primitive.count && out->vertices.size() < PoseSkin::MAX_VERTICES; v += stride){
                // sparse accessors (substituted values) and accessors without a buffer view are
                // not read: their vertices are left out of the sample (readAccessor fails)
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

    std::vector<Matrix4> inverseBind(skeleton.bones.size(), Matrix4());
    for (size_t i = 0; i < boneEntities.size() && i < skeleton.bones.size(); i++){
        if (const BoneComponent* bone = scene->findComponent<BoneComponent>(boneEntities[i])){
            inverseBind[i] = bone->offsetMatrix;
        }
    }
    return sampleSkin(gltf, [&](int skinIndex, size_t joint){
        const tinygltf::Skin& skin = gltf.skins[skinIndex];
        auto bone = modelComponent->bonesIdMapping.find(skin.joints[joint]);
        if (bone == modelComponent->bonesIdMapping.end()) return -1;
        auto index = indexOf.find(bone->second);
        return index != indexOf.end() ? index->second : -1;
    }, inverseBind);
}

bool pose::buildSkeletonFromGltf(const tinygltf::Model& gltf, Skeleton& skeleton, std::vector<std::string>& bonePaths,
                                 std::vector<Matrix4>* bridges, std::vector<int>* boneNodes){
    skeleton.bones.clear();
    bonePaths.clear();
    if (bridges) bridges->clear();
    if (boneNodes) boneNodes->clear();
    const int count = (int)gltf.nodes.size();

    // the joints of every skin, in node order (the loader's bonesIdMapping order)
    std::vector<bool> isBone(count, false);
    for (const tinygltf::Skin& skin : gltf.skins){
        for (int joint : skin.joints){
            if (joint >= 0 && joint < count) isBone[joint] = true;
        }
    }
    // loadGLTF's parents: the last node listing a node as a child
    std::vector<int> parents(count, -1);
    for (int i = 0; i < count; i++){
        for (int child : gltf.nodes[i].children){
            if (child >= 0 && child < count) parents[child] = i;
        }
    }
    std::vector<int> nodes;
    std::vector<int> indexOf(count, -1);
    for (int i = 0; i < count; i++){
        if (!isBone[i]) continue;
        indexOf[i] = (int)nodes.size();
        nodes.push_back(i);
    }
    if (nodes.empty()) return false;

    // the nodes' local TRS as the entities get them (the node matrix decomposed)
    struct TRS { Vector3 position; Quaternion rotation; Vector3 scale; };
    std::vector<TRS> locals(count);
    std::vector<bool> decomposed(count, false);
    auto local = [&](int node) -> const TRS& {
        if (!decomposed[node]){
            gltfNodeMatrix(gltf, node).decompose(locals[node].position, locals[node].scale, locals[node].rotation);
            decomposed[node] = true;
        }
        return locals[node];
    };

    const std::vector<std::string> paths = anim::modelNodePaths(gltf);
    skeleton.bones.resize(nodes.size());
    if (bridges) bridges->assign(nodes.size(), Matrix4());
    for (size_t i = 0; i < nodes.size(); i++){
        const int node = nodes[i];
        SkeletonBone& out = skeleton.bones[i];
        out.name = gltf.nodes[node].name.empty() ? "Node " + std::to_string(node) : gltf.nodes[node].name;
        out.parent = -1;
        bonePaths.push_back((size_t)node < paths.size() ? paths[node] : out.name);

        // non-bone nodes between the bone and its parent bone (or the model root) are folded in
        Matrix4 bridge;
        bool hasBridge = false;
        int parent = parents[node];
        int guard = 0;
        while (parent >= 0 && guard++ < 1024){
            if (indexOf[parent] >= 0){
                out.parent = indexOf[parent];
                break;
            }
            const TRS& trs = local(parent);
            bridge = trsMatrix(trs.position, trs.rotation, trs.scale) * bridge;
            hasBridge = true;
            parent = parents[parent];
        }

        const TRS& rest = local(node);
        if (hasBridge){
            const Matrix4 folded = bridge * trsMatrix(rest.position, rest.rotation, rest.scale);
            folded.decompose(out.position, out.scale, out.rotation);
            if (bridges) (*bridges)[i] = bridge;
        }else{
            out.position = rest.position;
            out.rotation = rest.rotation;
            out.scale = rest.scale;
        }
    }
    if (boneNodes) *boneNodes = nodes;
    return true;
}

void pose::gltfNodeLocal(const tinygltf::Model& gltf, int node, Vector3& position, Quaternion& rotation, Vector3& scale){
    gltfNodeMatrix(gltf, node).decompose(position, scale, rotation);
}

std::shared_ptr<PoseSkin> pose::buildSkinFromGltf(const tinygltf::Model& gltf, const Skeleton& skeleton,
                                                  const std::vector<int>& boneNodes){
    std::unordered_map<int, int> boneOfNode;
    for (size_t i = 0; i < boneNodes.size(); i++) boneOfNode[boneNodes[i]] = (int)i;
    // the inverse bind of the first skin listing each joint (the loader's BoneComponent)
    std::vector<Matrix4> inverseBind(skeleton.bones.size(), Matrix4());
    std::vector<bool> seen(skeleton.bones.size(), false);
    for (size_t s = 0; s < gltf.skins.size(); s++){
        for (size_t j = 0; j < gltf.skins[s].joints.size(); j++){
            auto bone = boneOfNode.find(gltf.skins[s].joints[j]);
            if (bone == boneOfNode.end() || (size_t)bone->second >= seen.size() || seen[(size_t)bone->second]) continue;
            seen[(size_t)bone->second] = true;
            inverseBind[(size_t)bone->second] = gltfInverseBind(gltf, (int)s, j);
        }
    }
    return sampleSkin(gltf, [&](int skinIndex, size_t joint){
        auto bone = boneOfNode.find(gltf.skins[skinIndex].joints[joint]);
        return bone != boneOfNode.end() ? bone->second : -1;
    }, inverseBind);
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

bool pose::resolveHumanoid(const Skeleton& skeleton, const HumanoidOverrides& overrides, HumanoidMap& out,
                           HumanExtraMap& extra, std::string* error, std::vector<std::string>* notes){
    extra = HumanExtraMap();
    if (!resolveHumanoid(skeleton, overrides, out, error, notes)) return false;
    return resolveExtraBones(skeleton, out, overrides.extraBones, extra, error, notes);
}

bool pose::humanoidSidecarStamp(const std::string& modelPath, std::string& stamp, std::string* error){
    stamp.clear();
    bool notFound = false;
    if (readTextFile(humanoidSidecarPath(modelPath), stamp, error, &notFound)) return true;
    stamp.clear();
    if (!notFound) return false;
    // no sidecar: all auto-detect
    if (error) error->clear();
    return true;
}

PoseRigCache& PoseRigCache::shared(){
    static PoseRigCache cache;
    return cache;
}

bool PoseRigCache::find(const std::string& modelPath, const std::string& sidecarStamp, const std::shared_ptr<const void>& source,
                        const Skeleton& skeleton, ModelRigInfo& out){
    std::lock_guard<std::mutex> lock(mutex);
    pruneLocked();
    for (const Entry& entry : entries){
        if (entry.modelPath != modelPath || entry.stamp != sidecarStamp) continue;
        if (entry.hasSource != (source != nullptr)) continue;
        if (entry.hasSource && entry.source.lock() != source) continue;
        if (!sameSkeleton(entry.skeleton, skeleton)) continue;
        out = entry.info;
        return true;
    }
    return false;
}

bool PoseRigCache::get(const std::string& modelPath, const std::string& sidecarStamp, const std::shared_ptr<const void>& source,
                       const Skeleton& skeleton, const HumanoidOverrides& overrides,
                       const std::function<std::shared_ptr<PoseSkin>()>& skinFactory, ModelRigInfo& out, std::string* error){
    if (find(modelPath, sidecarStamp, source, skeleton, out)) return true;

    // built without the lock: the skin factory reads the scene, and builds take a while
    ModelRigInfo info;
    if (!resolveHumanoid(skeleton, overrides, info.humanoid, info.extra, error, &info.notes)) return false;
    info.animationType = overrides.animationType.value_or(AnimationType::Generic);
    info.overrides = overrides;
    std::shared_ptr<PoseSkin> skin = skinFactory ? skinFactory() : nullptr;
    auto rig = std::make_shared<PoseRig>();
    // the skin first: it sets the floor and the sole offsets the neutral pose is grounded with
    if (skin) rig->setSkin(skin);
    const std::array<float, HUMAN_BONE_COUNT> twist = overrides.twistDegrees();
    if (!rig->build(skeleton, info.humanoid, error, &twist)) return false;
    info.skin = skin;
    info.rig = rig;

    std::lock_guard<std::mutex> lock(mutex);
    // one rig per model file: an older sidecar, model data or skeleton is replaced
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& entry){ return entry.modelPath == modelPath; }), entries.end());
    Entry entry;
    entry.modelPath = modelPath;
    entry.stamp = sidecarStamp;
    entry.source = source;
    entry.hasSource = source != nullptr;
    entry.skeleton = skeleton;
    entry.info = info;
    entries.push_back(std::move(entry));
    out = info;
    return true;
}

void PoseRigCache::pruneLocked(){
    // the data of a reloaded or unloaded model is gone: its rigs too
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry& entry){ return entry.hasSource && entry.source.expired(); }), entries.end());
}

void PoseRigCache::prune(){
    std::lock_guard<std::mutex> lock(mutex);
    pruneLocked();
}

void PoseRigCache::clear(){
    std::lock_guard<std::mutex> lock(mutex);
    entries.clear();
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

    // the sidecar next to the model file; missing = all auto-detect, unreadable = an error
    std::string stamp;
    if (!humanoidSidecarStamp(modelComponent->filename, stamp, error)) return false;
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
    // the loaded data identifies the file's content: a reload is a new rig
    std::shared_ptr<const void> source;
    if (modelComponent->gltfModel) source = modelComponent->gltfModel;
    else if (modelComponent->objModel) source = modelComponent->objModel;
    if (!PoseRigCache::shared().get(modelPath, stamp, source, skeleton, overrides,
            [&]() { return buildSkin(scene, model, skeleton, boneEntities); }, out, error)){
        if (error && !error->empty()) *error = modelPath + ": " + *error;
        return false;
    }
    for (const std::string& warning : warnings){
        out.notes.push_back(humanoidSidecarPath(modelPath) + ": " + warning);
    }
    return true;
}
