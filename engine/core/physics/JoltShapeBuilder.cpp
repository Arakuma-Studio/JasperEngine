// SPDX-License-Identifier: MIT

#include "JoltShapeBuilder.h"

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "Jolt/RegisterTypes.h"
#include "Jolt/Core/Factory.h"
#include "Jolt/Physics/PhysicsSettings.h"
#include "Jolt/Physics/Collision/Shape/BoxShape.h"
#include "Jolt/Physics/Collision/Shape/CapsuleShape.h"
#include "Jolt/Physics/Collision/Shape/ConvexHullShape.h"
#include "Jolt/Physics/Collision/Shape/CylinderShape.h"
#include "Jolt/Physics/Collision/Shape/HeightFieldShape.h"
#include "Jolt/Physics/Collision/Shape/MeshShape.h"
#include "Jolt/Physics/Collision/Shape/SphereShape.h"
#include "Jolt/Physics/Collision/Shape/TaperedCapsuleShape.h"
#endif

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

using namespace jasperengine;

namespace {

    Vector3 absScale(const Vector3& scale){
        return Vector3(std::fabs(scale.x), std::fabs(scale.y), std::fabs(scale.z));
    }

    float maxScaleXZ(const Vector3& scale){
        return std::max(scale.x, scale.z);
    }

    float maxScaleXYZ(const Vector3& scale){
        return std::max(scale.x, std::max(scale.y, scale.z));
    }

    void setError(std::string* error, const std::string& message){
        if (error) *error = message;
    }

}

JoltShapeBuilder::VertexRange JoltShapeBuilder::vertexRange(const MeshSourceView& view){
    return VertexRange(view.vertexBuffer->getData(), view.vertexAttr.getOffset(), view.vertexAttr.getCount());
}

std::vector<JoltShapeBuilder::MeshSourceView> JoltShapeBuilder::collectMeshSourceViews(MeshComponent* mesh){
    std::vector<MeshSourceView> views;
    if (!mesh) return views;

    std::map<std::string, Buffer*> buffers;
    if (mesh->buffer.getSize() > 0) buffers["vertices"] = &mesh->buffer;
    if (mesh->indices.getSize() > 0) buffers["indices"] = &mesh->indices;
    for (int i = 0; i < mesh->numExternalBuffers; i++) buffers[mesh->eBuffers[i].getName()] = &mesh->eBuffers[i];

    MeshSourceView view;
    for (auto const& buf : buffers){
        if (buf.second->getAttribute(AttributeType::POSITION)){
            view.vertexBuffer = buf.second;
            view.vertexAttr = *buf.second->getAttribute(AttributeType::POSITION);
        }
        if (buf.second->getAttribute(AttributeType::INDEX)){
            view.indexBuffer = buf.second;
            view.indexAttr = *buf.second->getAttribute(AttributeType::INDEX);
        }
    }

    // An empty name keeps the buffer already in view; an unknown one drops the
    // attribute, whose offset only means anything against its own buffer.
    auto resolveNamedBuffer = [&buffers](const std::string& bufferName, Buffer* current) -> Buffer* {
        if (bufferName.empty()) return current;
        auto named = buffers.find(bufferName);
        return named != buffers.end() ? named->second : NULL;
    };

    for (size_t i = 0; i < mesh->numSubmeshes; i++){
        for (auto const& attr : mesh->submeshes[i].attributes){
            if (attr.first == AttributeType::POSITION){
                if (Buffer* resolved = resolveNamedBuffer(attr.second.getBufferName(), view.vertexBuffer)){
                    view.vertexBuffer = resolved;
                    view.vertexAttr = attr.second;
                }
            }
            if (attr.first == AttributeType::INDEX){
                if (Buffer* resolved = resolveNamedBuffer(attr.second.getBufferName(), view.indexBuffer)){
                    view.indexBuffer = resolved;
                    view.indexAttr = attr.second;
                }
            }
        }

        if (view.vertexBuffer) views.push_back(view);
    }

    return views;
}

std::vector<Entity> JoltShapeBuilder::meshSourceEntities(Scene* scene, Entity meshEntity){
    std::vector<Entity> sources;

    MeshComponent* mesh = scene->findComponent<MeshComponent>(meshEntity);
    if (mesh && mesh->numSubmeshes > 0){
        sources.push_back(meshEntity);
        return sources;
    }

    if (ModelComponent* model = scene->findComponent<ModelComponent>(meshEntity)){
        for (auto const& node : model->meshNodesMapping){
            MeshComponent* nodeMesh = scene->findComponent<MeshComponent>(node.second);
            if (nodeMesh && nodeMesh->numSubmeshes > 0){
                sources.push_back(node.second);
            }
        }
    }

    return sources;
}

Matrix4 JoltShapeBuilder::meshToBodyMatrix(Scene* scene, Entity bodyEntity, Entity meshEntity){
    Transform* meshTransform = scene->findComponent<Transform>(meshEntity);
    Transform* bodyTransform = scene->findComponent<Transform>(bodyEntity);

    if (meshEntity == bodyEntity || !meshTransform || !bodyTransform){
        return Matrix4::scaleMatrix(entityWorldScale(scene, meshEntity));
    }

    return bodyTransform->worldRotation.inverse().getRotationMatrix() *
           Matrix4::translateMatrix(-bodyTransform->worldPosition) *
           meshTransform->modelMatrix;
}

Vector3 JoltShapeBuilder::entityWorldScale(Scene* scene, Entity entity){
    Transform* transform = scene->findComponent<Transform>(entity);
    return transform ? transform->worldScale : Vector3::UNIT_SCALE;
}

bool JoltShapeBuilder::appendHullPoints(Scene* scene, Entity bodyEntity, Entity meshEntity, std::vector<Vector3>& points){
    std::vector<Entity> sourceEntities = meshSourceEntities(scene, meshEntity);
    if (sourceEntities.empty()){
        return false;
    }

    for (Entity sourceEntity : sourceEntities){
        const Matrix4 toBody = meshToBodyMatrix(scene, bodyEntity, sourceEntity);
        std::set<VertexRange> addedRanges;

        for (MeshSourceView& view : collectMeshSourceViews(scene->findComponent<MeshComponent>(sourceEntity))){
            if (!addedRanges.insert(vertexRange(view)).second) continue;

            int verticesize = int(view.vertexAttr.getCount());
            for (int i = 0; i < verticesize; i++){
                points.push_back(toBody * view.vertexBuffer->getVector3(&view.vertexAttr, i));
            }
        }
    }

    return true;
}

bool JoltShapeBuilder::appendTriangles(Scene* scene, Entity bodyEntity, Entity meshEntity,
                                       std::vector<Vector3>& vertices, std::vector<uint32_t>& indices){
    std::vector<Entity> sourceEntities = meshSourceEntities(scene, meshEntity);
    if (sourceEntities.empty()){
        return false;
    }

    for (Entity sourceEntity : sourceEntities){
        const Matrix4 toBody = meshToBodyMatrix(scene, bodyEntity, sourceEntity);
        std::map<VertexRange, uint32_t> vertexBases;

        for (MeshSourceView& view : collectMeshSourceViews(scene->findComponent<MeshComponent>(sourceEntity))){
            const VertexRange range = vertexRange(view);
            const int verticesize = int(view.vertexAttr.getCount());

            auto base = vertexBases.find(range);
            if (base == vertexBases.end()){
                base = vertexBases.emplace(range, (uint32_t)vertices.size()).first;
                for (int i = 0; i < verticesize; i++){
                    vertices.push_back(toBody * view.vertexBuffer->getVector3(&view.vertexAttr, i));
                }
            }

            // Indices are local to their submesh, rebase onto its vertices
            const uint32_t vertexBase = base->second;

            if (!view.indexBuffer){
                // No index buffer means a plain triangle list
                for (int i = 0; i + 2 < verticesize; i += 3){
                    indices.push_back(vertexBase + i);
                    indices.push_back(vertexBase + i + 1);
                    indices.push_back(vertexBase + i + 2);
                }
                continue;
            }

            int numIdxTriangles = int(view.indexAttr.getCount() / 3);
            for (int i = 0; i < numIdxTriangles; i++){
                for (int j = 0; j < 3; j++){
                    uint32_t indice = 0;
                    if (view.indexAttr.getDataType() == AttributeDataType::UNSIGNED_INT){
                        indice = view.indexBuffer->getUInt32(&view.indexAttr, (3 * i) + j);
                    }else if (view.indexAttr.getDataType() == AttributeDataType::UNSIGNED_SHORT){
                        indice = view.indexBuffer->getUInt16(&view.indexAttr, (3 * i) + j);
                    }
                    indices.push_back(vertexBase + indice);
                }
            }
        }
    }

    return true;
}

bool JoltShapeBuilder::isGeometryLoading(Scene* scene, Entity entity){
    Entity current = entity;

    while (current != NULL_ENTITY){
        ModelComponent* model = scene->findComponent<ModelComponent>(current);
        if (model && !model->filename.empty() && model->needUpdateModel){
            return true;
        }

        TerrainComponent* terrain = scene->findComponent<TerrainComponent>(current);
        if (terrain && !terrain->heightMap.empty() && terrain->needUpdateTerrain){
            return true;
        }

        Transform* transform = scene->findComponent<Transform>(current);
        current = transform ? transform->parent : NULL_ENTITY;
    }

    return false;
}

#ifdef JASPERENGINE_PHYSICS_3D
void JoltShapeBuilder::ensureJoltGlobalInit(){
    static const bool done = [](){
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        return true;
    }();
    (void)done;
}

// Intentionally leaked: the editor keeps its App (and every Scene) in a static, so
// ~PhysicsSystem runs during static destruction and must not find these already gone.
JPH::TempAllocatorImpl& JoltShapeBuilder::sharedPhysicsTempAllocator(){
    static JPH::TempAllocatorImpl* instance = new JPH::TempAllocatorImpl(10 * 1024 * 1024);
    return *instance;
}

JPH::JobSystemThreadPool& JoltShapeBuilder::sharedPhysicsJobSystem(){
    // hardware_concurrency() is unsigned and reports 0 when unknown, so clamp
    // instead of subtracting into a huge worker count on a single-core machine.
    const unsigned int cores = JPH::thread::hardware_concurrency();
    const int workers = (cores > 1) ? (int)(cores - 1) : 1;
    static JPH::JobSystemThreadPool* instance =
        new JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, workers);
    return *instance;
}

JPH::ShapeRefC JoltShapeBuilder::buildConvexHullShape(const std::vector<Vector3>& points, std::string* error){
    (void)error;
    JPH::Array<JPH::Vec3> jvertices;
    jvertices.reserve(points.size());
    for (const Vector3& point : points){
        jvertices.push_back(JPH::Vec3(point.x, point.y, point.z));
    }

    JPH::ConvexHullShapeSettings shape_settings(jvertices);
    JPH::ShapeSettings::ShapeResult shape_result = shape_settings.Create();
    if (!shape_result.IsValid()) return NULL;
    return shape_result.Get();
}

JPH::ShapeRefC JoltShapeBuilder::buildMeshShape(const std::vector<Vector3>& vertices, const std::vector<uint32_t>& indices,
                                                std::string* error){
    (void)error;
    JPH::VertexList jvertices;
    jvertices.reserve(vertices.size());
    for (const Vector3& vertex : vertices){
        jvertices.push_back(JPH::Float3(vertex.x, vertex.y, vertex.z));
    }

    JPH::IndexedTriangleList jindices;
    jindices.reserve(indices.size() / 3);
    for (size_t i = 0; i + 2 < indices.size(); i += 3){
        jindices.push_back(JPH::IndexedTriangle(indices[i], indices[i + 1], indices[i + 2]));
    }

    JPH::MeshShapeSettings shape_settings(jvertices, jindices);
    JPH::ShapeSettings::ShapeResult shape_result = shape_settings.Create();
    if (!shape_result.IsValid()) return NULL;
    return shape_result.Get();
}

JPH::ShapeRefC JoltShapeBuilder::buildHeightFieldShape(TerrainComponent& terrain, unsigned int samplesSize, std::string* error){
    if (!terrain.heightMapLoaded){
        setError(error, "Cannot create heightfield shape without loaded terrain heightmap");
        return NULL;
    }

    TextureData& textureData = terrain.heightMap.getData();
    if (samplesSize == 0){
        samplesSize = textureData.getMinNearestPowerOfTwo();
        if (samplesSize > std::min(textureData.getOriginalWidth(), textureData.getOriginalHeight())){
            samplesSize = samplesSize / 2;
        }
    }

    JPH::Vec3 terrainOffset = JPH::Vec3(-terrain.terrainSize / 2.0, 0.0f, -terrain.terrainSize / 2.0);
    JPH::Vec3 terrainScale = JPH::Vec3(terrain.terrainSize / samplesSize, terrain.maxHeight, terrain.terrainSize / samplesSize);

    const unsigned char* heightPixels = static_cast<const unsigned char*>(textureData.getData());
    const int heightChannels = textureData.getChannels();
    const int heightBytesPerChannel = TextureData::getBytesPerChannel(textureData.getColorFormat());

    std::vector<float> samples(samplesSize * samplesSize);
    for (unsigned int x = 0; x < samplesSize; x++){
        for (unsigned int y = 0; y < samplesSize; y++){
            int posX = floor(textureData.getWidth() * x / samplesSize);
            int posY = floor(textureData.getHeight() * y / samplesSize);
            const size_t texelIndex = (static_cast<size_t>(posY) * textureData.getWidth() + posX);
            const size_t index = texelIndex * static_cast<size_t>(heightChannels) * static_cast<size_t>(heightBytesPerChannel);
            float val;
            if (heightBytesPerChannel >= 2){
                const unsigned int raw = static_cast<unsigned int>(heightPixels[index]) |
                                         (static_cast<unsigned int>(heightPixels[index + 1]) << 8);
                val = static_cast<float>(raw) / 65535.0f;
            }else{
                val = heightPixels[index] / 255.0f;
            }
            samples[x + (y * samplesSize)] = val;
        }
    }

    JPH::HeightFieldShapeSettings shape_settings(samples.data(), terrainOffset, terrainScale, samplesSize);
    JPH::ShapeSettings::ShapeResult shape_result = shape_settings.Create();
    if (!shape_result.IsValid()) return NULL;
    return shape_result.Get();
}

JPH::ShapeRefC JoltShapeBuilder::buildColliderShape(Scene* scene, Entity entity, const Collider3D& shapeData, std::string* error){
    const Vector3 entityScale = entityWorldScale(scene, entity);
    const Vector3 absEntityScale = absScale(entityScale);

    if (shapeData.type == Collider3DType::BOX){
        JPH::BoxShapeSettings shape_settings(JPH::Vec3(shapeData.width * absEntityScale.x / 2.0f, shapeData.height * absEntityScale.y / 2.0f, shapeData.depth * absEntityScale.z / 2.0f));
        JPH::ShapeSettings::ShapeResult shape_result = shape_settings.Create();
        if (!shape_result.IsValid()) return NULL;
        return shape_result.Get();
    }else if (shapeData.type == Collider3DType::SPHERE){
        JPH::SphereShapeSettings shape_settings(shapeData.radius * maxScaleXYZ(absEntityScale));
        JPH::ShapeSettings::ShapeResult shape_result = shape_settings.Create();
        if (!shape_result.IsValid()) return NULL;
        return shape_result.Get();
    }else if (shapeData.type == Collider3DType::CAPSULE){
        JPH::CapsuleShapeSettings shape_settings(shapeData.halfHeight * absEntityScale.y, shapeData.radius * maxScaleXZ(absEntityScale));
        JPH::ShapeSettings::ShapeResult shape_result = shape_settings.Create();
        if (!shape_result.IsValid()) return NULL;
        return shape_result.Get();
    }else if (shapeData.type == Collider3DType::TAPERED_CAPSULE){
        const float radiusScale = maxScaleXZ(absEntityScale);
        JPH::TaperedCapsuleShapeSettings shape_settings(shapeData.halfHeight * absEntityScale.y, shapeData.topRadius * radiusScale, shapeData.bottomRadius * radiusScale);
        JPH::ShapeSettings::ShapeResult shape_result = shape_settings.Create();
        if (!shape_result.IsValid()) return NULL;
        return shape_result.Get();
    }else if (shapeData.type == Collider3DType::CYLINDER){
        JPH::CylinderShapeSettings shape_settings(shapeData.halfHeight * absEntityScale.y, shapeData.radius * maxScaleXZ(absEntityScale));
        JPH::ShapeSettings::ShapeResult shape_result = shape_settings.Create();
        if (!shape_result.IsValid()) return NULL;
        return shape_result.Get();
    }else if (shapeData.type == Collider3DType::CONVEX_HULL){
        const Collider3DSource source = shapeData.source == Collider3DSource::NONE ? Collider3DSource::ENTITY_MESH : shapeData.source;
        std::vector<Vector3> points;

        if (source == Collider3DSource::RAW_VERTICES){
            points.resize(shapeData.numVertices);
            for (size_t i = 0; i < shapeData.numVertices; i++){
                points[i] = shapeData.vertices[i] * entityScale;
            }
        }else if (source == Collider3DSource::ENTITY_MESH){
            Entity meshEntity = shapeData.sourceEntity == NULL_ENTITY ? entity : shapeData.sourceEntity;
            if (!appendHullPoints(scene, entity, meshEntity, points)){
                setError(error, "Cannot create convex hull shape: no mesh geometry found for entity " + std::to_string(meshEntity));
                return NULL;
            }
        }

        if (points.empty()){
            setError(error, "Cannot create convex hull shape without vertices for 3D Body entity: " + std::to_string(entity));
            return NULL;
        }

        return buildConvexHullShape(points, error);
    }else if (shapeData.type == Collider3DType::MESH){
        const Collider3DSource source = shapeData.source == Collider3DSource::NONE ? Collider3DSource::ENTITY_MESH : shapeData.source;
        std::vector<Vector3> vertices;
        std::vector<uint32_t> indices;

        if (source == Collider3DSource::RAW_MESH){
            vertices.resize(shapeData.numVertices);
            for (size_t i = 0; i < shapeData.numVertices; i++){
                vertices[i] = shapeData.vertices[i] * entityScale;
            }

            int indicesize = int(shapeData.numIndices / 3);
            for (int i = 0; i < (indicesize * 3); i++){
                indices.push_back(shapeData.indices[i]);
            }
        }else if (source == Collider3DSource::ENTITY_MESH){
            Entity meshEntity = shapeData.sourceEntity == NULL_ENTITY ? entity : shapeData.sourceEntity;
            if (!appendTriangles(scene, entity, meshEntity, vertices, indices)){
                setError(error, "Cannot create mesh shape: no mesh geometry found for entity " + std::to_string(meshEntity));
                return NULL;
            }
        }

        if (vertices.empty() || indices.empty()){
            setError(error, "Cannot create mesh shape without vertices and indices for 3D Body entity: " + std::to_string(entity));
            return NULL;
        }

        return buildMeshShape(vertices, indices, error);
    }else if (shapeData.type == Collider3DType::HEIGHTFIELD){
        Entity terrainEntity = shapeData.sourceEntity == NULL_ENTITY ? entity : shapeData.sourceEntity;
        TerrainComponent* terrain = scene->findComponent<TerrainComponent>(terrainEntity);
        if (!terrain){
            setError(error, "Cannot create heightfield shape without loaded terrain heightmap");
            return NULL;
        }

        return buildHeightFieldShape(*terrain, shapeData.samplesSize, error);
    }

    return NULL;
}
#endif
