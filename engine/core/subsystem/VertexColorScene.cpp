// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R1, R2, R9, R83, D12: the targets of a scene's entities and their
// CPU geometry. A Model's targets are keyed by glTF node and primitive: the loader fills a node's
// submeshes in primitive order and then reverses them (MeshSystem::getSubmeshRuns), so primitive
// p of a node of n primitives is submesh n - 1 - p of the mesh that draws the node. Merge Static
// Meshes, OBJ models and plain meshes are keyed by submesh index.

#include "subsystem/VertexColorScene.h"
#include "subsystem/VertexColorSceneDetail.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/VertexColorRenderer.h"

#include "Log.h"
#include "Scene.h"
#include "buffer/Buffer.h"
#include "component/MeshComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/ModelComponent.h"
#include "component/SplineExtrudeComponent.h"
#include "component/SpriteComponent.h"
#include "component/TerrainComponent.h"
#include "component/TextComponent.h"
#include "component/TilemapComponent.h"
#include "component/TilemapLayerComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"
#include "component/VertexColorsComponent.h"
#include "component/VoxelVolumeComponent.h"
#include "io/Data.h"
#include "model/ModelCpuData.h"
#include "texture/TexturePrepare.h"
#include "tiny_gltf.h"
#include "util/ContentHash.h"
#include "vertexcolor/VertexColorFile.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace jasperengine::vertexcolor::detail {

    namespace {

        const Buffer* vcSceneBuffer(const MeshComponent& mesh, const std::string& name) {
            if (name == "vertices") return &mesh.buffer;
            if (name == "indices") return &mesh.indices;
            for (unsigned int i = 0; i < mesh.numExternalBuffers; i++) {
                if (mesh.eBuffers[i].getName() == name) return &mesh.eBuffers[i];
            }
            return nullptr;
        }

        size_t vcSceneTypeSize(AttributeDataType type) {
            switch (type) {
                case AttributeDataType::BYTE:
                case AttributeDataType::UNSIGNED_BYTE: return 1;
                case AttributeDataType::SHORT:
                case AttributeDataType::UNSIGNED_SHORT:
                case AttributeDataType::HALF: return 2;
                default: return 4;
            }
        }

        uint64_t vcSceneMix(uint64_t h, uint64_t v) {
            h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
            return h;
        }

        uint64_t vcSceneBufferStamp(uint64_t h, const Buffer& buffer) {
            // the parse a model's bytes come from outlives a game freeing them; other bytes are their address
            if (buffer.hasShareSource()) {
                h = vcSceneMix(h, (uint64_t)(uintptr_t)buffer.getShareOwner().lock().get());
                h = vcSceneMix(h, buffer.getShareId());
            } else if (!buffer.isDataReleased() && buffer.getData()) {
                // a generated mesh's bytes (a primitive rebuilt in place keeps their address)
                h = vcSceneMix(h, hashContent64(buffer.getData(), buffer.getSize()));
            }
            h = vcSceneMix(h, buffer.getSize());
            h = vcSceneMix(h, buffer.getCount());
            return vcSceneMix(h, buffer.getStride());
        }

        // the glTF node of each of a Model's meshes, and whether its root mesh holds merged nodes
        struct VcModelLayout {
            std::vector<int> meshNodes;     // nodes with a mesh, ascending (MeshSystem's collectGLTFNodes)
            bool merged = false;
        };

        VcModelLayout vcModelLayout(const ModelComponent& model) {
            VcModelLayout layout;
            if (model.gltfModel) {
                for (size_t i = 0; i < model.gltfModel->nodes.size(); i++) {
                    if (model.gltfModel->nodes[i].mesh >= 0) layout.meshNodes.push_back((int)i);
                }
            }
            layout.merged = model.loadedMergeStaticMeshes && layout.meshNodes.size() > 1;
            return layout;
        }

        std::string vcNodeName(const ModelComponent& model, int node) {
            if (model.gltfModel && node >= 0 && node < (int)model.gltfModel->nodes.size()) {
                const std::string& name = model.gltfModel->nodes[node].name;
                if (!name.empty()) return name;
            }
            return "Node " + std::to_string(node);
        }

        void vcFillTarget(SceneTarget& target, const MeshComponent& mesh, unsigned int submesh) {
            const Submesh& s = mesh.submeshes[submesh];
            target.submesh = submesh;
            target.vertexCount = submeshVertexCount(mesh, s);
            const AttributeSource index = findAttribute(mesh, s, AttributeType::INDEX);
            const uint32_t corners = index ? (index.attribute.getCount() ? index.attribute.getCount() : index.buffer->getCount()) : target.vertexCount;
            target.triangleCount = s.primitiveType == PrimitiveType::TRIANGLES ? corners / 3 : 0;
            target.hasAssetColors = (bool)findAttribute(mesh, s, AttributeType::COLOR);
            target.material = s.material.name.str();
            target.faceCulling = s.faceCulling;
        }

        void vcAppendTargets(std::vector<SceneTarget>& out, Entity meshEntity, const MeshComponent& mesh, int node, const std::string& name) {
            const unsigned int count = mesh.numSubmeshes;
            for (unsigned int s = 0; s < count; s++) {
                SceneTarget target;
                target.meshEntity = meshEntity;
                if (node >= 0) {
                    const int primitive = (int)(count - 1 - s);
                    target.key.node = node;
                    target.key.primitive = primitive;
                    target.name = name + " (node " + std::to_string(node) + ", primitive " + std::to_string(primitive) + ")";
                } else {
                    target.key.submesh = (int)s;
                    target.name = "Submesh " + std::to_string(s);
                }
                vcFillTarget(target, mesh, s);
                out.push_back(std::move(target));
            }
        }

    }

    AttributeSource findAttribute(const MeshComponent& mesh, const Submesh& submesh, AttributeType type) {
        AttributeSource source;
        auto it = submesh.attributes.find(type);
        if (it != submesh.attributes.end()) {
            source.buffer = vcSceneBuffer(mesh, it->second.getBufferName());
            source.attribute = it->second;
            return source;
        }
        const Buffer* buffers[] = {&mesh.buffer, &mesh.indices};
        for (const Buffer* buffer : buffers) {
            if (buffer->getSize() == 0 || !buffer->isRenderAttributes()) continue;
            const std::map<AttributeType, Attribute> attributes = buffer->getAttributes();
            auto found = attributes.find(type);
            if (found != attributes.end()) {
                source.buffer = buffer;
                source.attribute = found->second;
                return source;
            }
        }
        return source;
    }

    uint32_t submeshVertexCount(const MeshComponent& mesh, const Submesh& submesh) {
        const AttributeSource position = findAttribute(mesh, submesh, AttributeType::POSITION);
        if (!position) return 0;
        if (submesh.attributes.count(AttributeType::POSITION) && position.attribute.getCount() > 0)
            return position.attribute.getCount();
        return position.buffer->getCount();
    }

    bool readSubmeshIndices(const MeshComponent& mesh, const Submesh& submesh, uint32_t vertexCount, std::vector<uint32_t>& out) {
        out.clear();
        if (submesh.primitiveType != PrimitiveType::TRIANGLES) return false;
        const AttributeSource index = findAttribute(mesh, submesh, AttributeType::INDEX);
        if (!index) {
            if (vertexCount % 3 != 0) return false;
            out.resize(vertexCount);
            for (uint32_t i = 0; i < vertexCount; i++) out[i] = i;
            return true;
        }
        const uint32_t count = index.attribute.getCount() ? index.attribute.getCount() : index.buffer->getCount();
        const AttributeDataType type = index.attribute.getDataType();
        const size_t size = vcSceneTypeSize(type);
        const size_t offset = index.attribute.getOffset();
        const unsigned char* data = index.buffer->getData();
        if (!data || count % 3 != 0 || offset + (size_t)count * size > index.buffer->getSize()) return false;
        out.resize(count);
        for (uint32_t i = 0; i < count; i++) {
            const unsigned char* at = data + offset + (size_t)i * size;
            uint32_t v = 0;
            if (type == AttributeDataType::UNSIGNED_INT || type == AttributeDataType::INT) {
                std::memcpy(&v, at, 4);
            } else if (type == AttributeDataType::UNSIGNED_SHORT || type == AttributeDataType::SHORT) {
                uint16_t s;
                std::memcpy(&s, at, 2);
                v = s;
            } else {
                v = *at;
            }
            if (v >= vertexCount) return false;
            out[i] = v;
        }
        return true;
    }

    bool readAttribute(const AttributeSource& source, uint32_t vertex, unsigned int elements, float* out) {
        if (!source) return false;
        const Attribute& attribute = source.attribute;
        const AttributeDataType type = attribute.getDataType();
        const size_t size = vcSceneTypeSize(type);
        const unsigned int available = std::min(elements, attribute.getElements());
        const size_t stride = source.buffer->getStride() ? source.buffer->getStride() : attribute.getElements() * size;
        const size_t at = attribute.getOffset() + (size_t)vertex * stride;
        const unsigned char* data = source.buffer->getData();
        if (!data || at + available * size > source.buffer->getSize()) return false;
        const bool normalized = attribute.getNormalized();
        for (unsigned int e = 0; e < elements; e++) {
            if (e >= available) {
                out[e] = e == 3 ? 1.0f : 0.0f;
                continue;
            }
            const unsigned char* v = data + at + e * size;
            switch (type) {
                case AttributeDataType::FLOAT: std::memcpy(&out[e], v, 4); break;
                case AttributeDataType::HALF: { uint16_t h; std::memcpy(&h, v, 2); out[e] = halfToFloat(h); break; }
                case AttributeDataType::SHORT: { int16_t q; std::memcpy(&q, v, 2); out[e] = normalized ? std::max(q / 32767.0f, -1.0f) : (float)q; break; }
                case AttributeDataType::UNSIGNED_SHORT: { uint16_t q; std::memcpy(&q, v, 2); out[e] = normalized ? q / 65535.0f : (float)q; break; }
                case AttributeDataType::BYTE: { int8_t q; std::memcpy(&q, v, 1); out[e] = normalized ? std::max(q / 127.0f, -1.0f) : (float)q; break; }
                case AttributeDataType::UNSIGNED_BYTE: out[e] = normalized ? *v / 255.0f : (float)*v; break;
                default: return false;
            }
            if (!std::isfinite(out[e])) out[e] = 0.0f;
        }
        return true;
    }

    uint64_t geometryStamp(const MeshComponent& mesh, unsigned int submesh) {
        uint64_t h = 0x5643u;
        if (mesh.buffer.getSize() > 0) h = vcSceneBufferStamp(h, mesh.buffer);
        if (mesh.indices.getSize() > 0) h = vcSceneBufferStamp(h, mesh.indices);
        h = vcSceneMix(h, mesh.numExternalBuffers);
        for (unsigned int i = 0; i < mesh.numExternalBuffers; i++) h = vcSceneBufferStamp(h, mesh.eBuffers[i]);
        h = vcSceneMix(h, mesh.numSubmeshes);
        if (submesh < mesh.numSubmeshes) {
            const Submesh& s = mesh.submeshes[submesh];
            h = vcSceneMix(h, (uint64_t)s.primitiveType);
            h = vcSceneMix(h, s.attributes.size());
            for (const auto& [type, attribute] : s.attributes) {
                h = vcSceneMix(h, (uint64_t)type);
                h = vcSceneMix(h, attribute.getOffset());
                h = vcSceneMix(h, attribute.getCount());
            }
        }
        return h;
    }

    std::vector<SceneTarget> resolveTargets(Scene* scene, Entity entity) {
        std::vector<SceneTarget> out;
        if (!scene || !paintRefusal(scene, entity).empty()) return out;
        const ModelComponent* model = scene->findComponent<ModelComponent>(entity);
        const MeshComponent* own = scene->findComponent<MeshComponent>(entity);
        if (model && !model->filename.empty() && model->gltfModel) {
            if (!model->meshNodesMapping.empty()) {
                for (const auto& [node, child] : model->meshNodesMapping) {
                    const MeshComponent* mesh = scene->findComponent<MeshComponent>(child);
                    if (mesh) vcAppendTargets(out, child, *mesh, node, vcNodeName(*model, node));
                }
            } else if (own) {
                const VcModelLayout layout = vcModelLayout(*model);
                if (layout.merged || layout.meshNodes.empty()) {
                    vcAppendTargets(out, entity, *own, -1, "");
                } else {
                    vcAppendTargets(out, entity, *own, layout.meshNodes[0], vcNodeName(*model, layout.meshNodes[0]));
                }
            }
        } else if (own) {
            // a plain mesh, a primitive, an OBJ model
            vcAppendTargets(out, entity, *own, -1, "");
        }
        std::sort(out.begin(), out.end(), [](const SceneTarget& a, const SceneTarget& b) { return a.key < b.key; });
        return out;
    }

    bool resolveTarget(Scene* scene, Entity entity, const TargetKey& key, SceneTarget& out) {
        for (SceneTarget& target : resolveTargets(scene, entity)) {
            if (target.key == key) {
                out = std::move(target);
                return true;
            }
        }
        return false;
    }

    bool loadComponentData(Scene* scene, Entity entity, VertexColorsComponent& colors, bool* read) {
        if (read) *read = false;
        if (colors.data.empty()) return true;
        const std::string& path = colors.data.str();
        if (colors.loadedPath == path && (colors.loaded || colors.loadFailed)) return (bool)colors.loaded;
        jasperengine::Data file(path.c_str());
        std::string error;
        auto data = std::make_shared<vertexcolor::Data>();
        if (file.length() == 0 || !decode(file.getMemPtr(), file.length(), *data, error)) {
            Log::error("Vertex Colors on '%s' cannot read '%s'%s%s: the mesh draws its own colors", scene->getEntityName(entity).c_str(),
                       path.c_str(), error.empty() ? "" : ": ", error.c_str());
            colors.loaded.reset();
            colors.loadedPath = path;
            colors.loadFailed = true;
            return false;
        }
        colors.loaded = data;
        colors.loadedPath = path;
        colors.loadFailed = false;
        colors.revision++;
        if (read) *read = true;
        return true;
    }

    Data& writableData(Scene* scene, Entity entity, VertexColorsComponent& colors) {
        if (!colors.loaded) loadComponentData(scene, entity, colors);
        if (!colors.loaded) {
            colors.loaded = std::make_shared<Data>();
            colors.loaded->format = colors.format;
        } else if (colors.loaded.use_count() > 1) {
            colors.loaded = std::make_shared<Data>(*colors.loaded);
        }
        return *colors.loaded;
    }

    bool buildMeshView(const MeshComponent& mesh, unsigned int submeshIndex, MeshView& out, std::string& error) {
        out = MeshView();
        if (submeshIndex >= mesh.numSubmeshes) {
            error = "no such submesh";
            return false;
        }
        const Submesh& submesh = mesh.submeshes[submeshIndex];
        const uint32_t vertexCount = submeshVertexCount(mesh, submesh);
        const AttributeSource position = findAttribute(mesh, submesh, AttributeType::POSITION);
        if (!position || vertexCount == 0) {
            error = "the mesh has no positions";
            return false;
        }
        if (!readSubmeshIndices(mesh, submesh, vertexCount, out.indices)) {
            error = "the mesh is not a triangle list, or its indices are not readable";
            return false;
        }
        const AttributeSource normal = findAttribute(mesh, submesh, AttributeType::NORMAL);
        const AttributeSource uv0 = findAttribute(mesh, submesh, AttributeType::TEXCOORD1);
        const AttributeSource uv1 = findAttribute(mesh, submesh, AttributeType::TEXCOORD2);
        const AttributeSource color = findAttribute(mesh, submesh, AttributeType::COLOR);
        // an input that cannot be read whole is left out (face normals, no UVs, no asset colours)
        bool readNormals = (bool)normal, readUv0 = (bool)uv0, readUv1 = (bool)uv1, readColors = (bool)color;
        out.positions.resize(vertexCount);
        if (readNormals) out.normals.resize(vertexCount);
        if (readUv0) out.uv0.resize(vertexCount);
        if (readUv1) out.uv1.resize(vertexCount);
        if (readColors) out.assetColors.resize(vertexCount);
        float v[4];
        for (uint32_t i = 0; i < vertexCount; i++) {
            if (!readAttribute(position, i, 3, v)) {
                error = "the positions are not readable";
                return false;
            }
            out.positions[i] = Vector3(v[0], v[1], v[2]);
            if (readNormals) {
                readNormals = readAttribute(normal, i, 3, v);
                if (readNormals) out.normals[i] = Vector3(v[0], v[1], v[2]);
            }
            if (readUv0) {
                readUv0 = readAttribute(uv0, i, 2, v);
                if (readUv0) out.uv0[i] = Vector2(v[0], v[1]);
            }
            if (readUv1) {
                readUv1 = readAttribute(uv1, i, 2, v);
                if (readUv1) out.uv1[i] = Vector2(v[0], v[1]);
            }
            if (readColors) {
                // glTF COLOR_0 is linear; a VEC3 colour is opaque
                readColors = readAttribute(color, i, 4, v);
                if (readColors) out.assetColors[i] = Vector4(v[0], v[1], v[2], v[3]);
            }
        }
        if (!readNormals) out.normals.clear();
        if (!readUv0) out.uv0.clear();
        if (!readUv1) out.uv1.clear();
        if (!readColors) out.assetColors.clear();
        if (!out.build(&error)) return false;
        return true;
    }

}

namespace jasperengine::vertexcolor {

    std::string paintRefusal(Scene* scene, Entity entity) {
        if (!scene || !scene->isEntityCreated(entity)) return "the GameObject does not exist";
        if (!scene->findComponent<ModelComponent>(entity) && !scene->findComponent<MeshComponent>(entity))
            return "Vertex Colors go on a GameObject with a Model or a Mesh (add one first).";
        // editor/Catalog.cpp vertexColorsRefusal: the same generators, in the same order and words
        struct Generator { bool present; const char* name; };
        const Generator generators[] = {
            {scene->findComponent<SpriteComponent>(entity) != nullptr, "Sprite Renderer"},
            {scene->findComponent<TilemapComponent>(entity) != nullptr, "Free Tilemap (Legacy)"},
            {scene->findComponent<TilemapLayerComponent>(entity) != nullptr, "Tilemap Layer"},
            {scene->findComponent<MeshPolygonComponent>(entity) != nullptr, "Polygon Mesh"},
            {scene->findComponent<TerrainComponent>(entity) != nullptr, "Terrain"},
            {scene->findComponent<SplineExtrudeComponent>(entity) != nullptr, "Spline Extrude"},
            {scene->findComponent<VoxelVolumeComponent>(entity) != nullptr, "Voxel Volume"},
            {scene->findComponent<TextComponent>(entity) != nullptr, "Text"},
            {scene->findComponent<UIComponent>(entity) != nullptr, "Canvas Renderer"},
        };
        for (const Generator& generator : generators) {
            if (!generator.present) continue;
            const std::string name = generator.name;
            return "Vertex Colors cannot paint the mesh a " + name + " makes: the " + name +
                   " rebuilds it with its own colors. Paint a Model or a Mesh primitive.";
        }
        const Transform* transform = scene->findComponent<Transform>(entity);
        for (Entity parent = transform ? transform->parent : NULL_ENTITY; parent != NULL_ENTITY;) {
            if (const ModelComponent* model = scene->findComponent<ModelComponent>(parent)) {
                for (const auto& [node, mapped] : model->meshNodesMapping) {
                    if (mapped == entity)
                        return "Vertex Colors go on the Model's GameObject, not on one of its mesh nodes: a reload of the "
                               "model makes those again (select the Model's GameObject).";
                }
                break;
            }
            const Transform* parentTransform = scene->findComponent<Transform>(parent);
            parent = parentTransform ? parentTransform->parent : NULL_ENTITY;
        }
        return "";
    }

    std::vector<SceneTarget> sceneTargets(Scene* scene, Entity entity) {
        return detail::resolveTargets(scene, entity);
    }

    std::shared_ptr<const MeshView> sceneMeshView(Scene* scene, Entity entity, const TargetKey& key) {
        SceneTarget target;
        if (!detail::resolveTarget(scene, entity, key, target)) return nullptr;
        MeshComponent* mesh = scene->findComponent<MeshComponent>(target.meshEntity);
        RenderSystem* render = scene->findSystem<RenderSystem>().get();
        if (!mesh || !render) return nullptr;
        return render->getVertexColors().getMeshView(entity, target.meshEntity, target.submesh, *mesh);
    }

    const Material* sceneTargetMaterial(Scene* scene, const SceneTarget& target) {
        const MeshComponent* mesh = scene ? scene->findComponent<MeshComponent>(target.meshEntity) : nullptr;
        return mesh && target.submesh < mesh->numSubmeshes ? &mesh->submeshes[target.submesh].material : nullptr;
    }

    Matrix4 sceneTargetModel(Scene* scene, Entity entity, const TargetKey& key) {
        SceneTarget target;
        if (!detail::resolveTarget(scene, entity, key, target)) {
            const Transform* transform = scene ? scene->findComponent<Transform>(entity) : nullptr;
            return transform ? transform->modelMatrix : Matrix4();
        }
        const Transform* transform = scene->findComponent<Transform>(target.meshEntity);
        return transform ? transform->modelMatrix : Matrix4();
    }

    void markVertexColorsChanged(Scene* scene, Entity entity) {
        VertexColorsComponent* colors = scene ? scene->findComponent<VertexColorsComponent>(entity) : nullptr;
        if (colors) colors->revision++;
    }

    uint32_t boundSplitVertices(Scene* scene, Entity entity, const TargetKey& key) {
        RenderSystem* render = scene ? scene->findSystem<RenderSystem>().get() : nullptr;
        return render ? render->getVertexColors().getBoundSplitVertices(entity, key) : 0;
    }

    bool targetMatchesMesh(Scene* scene, Entity entity, const TargetKey& key, bool topologyOnly) {
        const VertexColorsComponent* colors = scene ? scene->findComponent<VertexColorsComponent>(entity) : nullptr;
        if (!colors || !colors->loaded) return false;
        const TargetData* data = colors->loaded->find(key);
        if (!data) return false;
        std::shared_ptr<const MeshView> view = sceneMeshView(scene, entity, key);
        if (!view) return false;
        const Fingerprint mesh = view->fingerprint();
        return topologyOnly ? data->fingerprint.sameTopology(mesh) : data->fingerprint == mesh;
    }

}
