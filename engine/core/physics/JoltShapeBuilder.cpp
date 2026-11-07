// SPDX-License-Identifier: MIT

#include "JoltShapeBuilder.h"

#include "Log.h"

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "object/TerrainEdit.h"
#include "terrain/TerrainHoles.h"
#include "subsystem/MeshSystem.h"
#include "util/MeshTriangles.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "memory/MemoryStats.h"
#include "util/JoltJobAdapter.h"
#include "Jolt/Core/JobSystemSingleThreaded.h"
#include "Jolt/RegisterTypes.h"
#include "Jolt/Core/Factory.h"
#include "Jolt/Physics/PhysicsSettings.h"
#include "Jolt/Physics/Collision/Shape/BoxShape.h"
#include "Jolt/Physics/Collision/Shape/CapsuleShape.h"
#include "Jolt/Physics/Collision/Shape/ConvexShape.h"
#include "Jolt/Physics/Collision/Shape/ConvexHullShape.h"
#include "Jolt/Physics/Collision/Shape/CylinderShape.h"
#include "Jolt/Physics/Collision/Shape/HeightFieldShape.h"
#include "Jolt/Physics/Collision/Shape/MeshShape.h"
#include "Jolt/Physics/Collision/Shape/SphereShape.h"
#include "Jolt/Physics/Collision/Shape/TaperedCapsuleShape.h"
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>

using namespace jasperengine;

namespace {
#ifdef JASPERENGINE_PHYSICS_3D

    Vector3 absScale(const Vector3& scale){
        return Vector3(std::fabs(scale.x), std::fabs(scale.y), std::fabs(scale.z));
    }
#endif
#ifdef JASPERENGINE_PHYSICS_3D

    float maxScaleXZ(const Vector3& scale){
        return std::max(scale.x, scale.z);
    }
#endif
#ifdef JASPERENGINE_PHYSICS_3D

    float maxScaleXYZ(const Vector3& scale){
        return std::max(scale.x, std::max(scale.y, scale.z));
    }
#endif

#ifdef JASPERENGINE_PHYSICS_3D
    // Jolt's default convex radius, or half the thinnest extent when that is thinner: a box
    // must be thicker than its radius (a cylinder at least as thick), so a thin plate or disc
    // would otherwise fail to build
    float fittingConvexRadius(float thinnestHalfExtent, bool strictlyInside){
        const bool fits = strictlyInside ? thinnestHalfExtent > JPH::cDefaultConvexRadius : thinnestHalfExtent >= JPH::cDefaultConvexRadius;
        return fits ? JPH::cDefaultConvexRadius : std::max(0.0f, 0.5f * thinnestHalfExtent);
    }
#endif
#ifdef JASPERENGINE_PHYSICS_3D

    void setError(std::string* error, const std::string& message){
        if (error) *error = message;
    }
#endif

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

Matrix4 JoltShapeBuilder::entityLinearMatrix(Scene* scene, Entity entity){
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!transform) return Matrix4();
    Matrix4 linear = transform->worldRotation.inverse().getRotationMatrix() * transform->modelMatrix;
    for (int r = 0; r < 3; r++) linear.set(3, r, 0.0f);
    return linear;
}

Vector3 JoltShapeBuilder::entityWorldScale(Scene* scene, Entity entity){
    Transform* transform = scene->findComponent<Transform>(entity);
    return transform ? transform->worldScale : Vector3::UNIT_SCALE;
}

bool JoltShapeBuilder::appendHullPoints(Scene* scene, Entity bodyEntity, Entity meshEntity, std::vector<Vector3>& points){
    std::vector<Entity> sourceEntities = MeshTriangles::meshSourceEntities(scene, meshEntity);
    if (sourceEntities.empty()){
        return false;
    }

    for (Entity sourceEntity : sourceEntities){
        const Matrix4 toBody = meshToBodyMatrix(scene, bodyEntity, sourceEntity);
        std::set<MeshTriangles::VertexRange> addedRanges;

        for (MeshTriangles::MeshSourceView& view : MeshTriangles::collectMeshSourceViews(scene->findComponent<MeshComponent>(sourceEntity))){
            if (!addedRanges.insert(MeshTriangles::vertexRange(view)).second) continue;

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
    std::vector<Entity> sourceEntities = MeshTriangles::meshSourceEntities(scene, meshEntity);
    if (sourceEntities.empty()){
        return false;
    }

    for (Entity sourceEntity : sourceEntities){
        MeshTriangles::appendMeshTriangles(scene->findComponent<MeshComponent>(sourceEntity), meshToBodyMatrix(scene, bodyEntity, sourceEntity), vertices, indices);
    }

    return true;
}

#ifdef JASPERENGINE_PHYSICS_3D
void JoltShapeBuilder::ensureJoltGlobalInit(){
    static const bool done = [](){
        JPH::RegisterDefaultAllocator();
#ifdef JPH_ENABLE_ASSERTS
        // Jolt's default handler breaks into a debugger (a trap that ends the process without one).
        // Its asserts include conditions it then reports and survives, such as an update whose
        // contact buffer filled (EPhysicsUpdateError, which the engine reports itself): logged,
        // once per place, and the step goes on
        JPH::AssertFailed = [](const char* expression, const char* message, const char* file, JPH::uint line) -> bool {
            static std::mutex mutex;
            static std::set<std::string>* seen = new std::set<std::string>();
            std::lock_guard<std::mutex> lock(mutex);
            if (seen->insert(std::string(file) + ":" + std::to_string(line)).second)
                Log::warn("Jolt assertion: %s%s%s (%s:%u)", expression, message ? ": " : "", message ? message : "", file, (unsigned int)line);
            return false;
        };
#endif
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        return true;
    }();
    (void)done;
}

// Intentionally leaked: the editor keeps its App (and every Scene) in a static, so
// ~PhysicsSystem runs during static destruction and must not find these already gone.
JPH::TempAllocator& JoltShapeBuilder::sharedPhysicsTempAllocator(){
    constexpr unsigned int bytes = 10 * 1024 * 1024;
    static JPH::TempAllocatorImplWithMallocFallback* instance = [](){
        // its block, reserved for the process, in the memory stats (the malloc fallback is not)
        static memory::TrackedBytes* tracked = new memory::TrackedBytes("Physics.JoltTempAllocator", memory::Residency::Cpu);
        tracked->add(bytes);
        tracked->setCapacity(bytes);
        return new JPH::TempAllocatorImplWithMallocFallback(bytes);
    }();
    return *instance;
}

JPH::JobSystem& JoltShapeBuilder::sharedPhysicsJobSystem(){
#ifdef JE_JOLT_OWN_POOL
    // hardware_concurrency() is unsigned and reports 0 when unknown, so clamp
    // instead of subtracting into a huge worker count on a single-core machine.
    const unsigned int cores = JPH::thread::hardware_concurrency();
    const int workers = (cores > 1) ? (int)(cores - 1) : 1;
    static JPH::JobSystemThreadPool* instance =
        new JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, workers);
#else
    static JoltJobAdapter* instance = new JoltJobAdapter(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers);
#endif
    return *instance;
}

JPH::JobSystem& JoltShapeBuilder::callingThreadPhysicsJobSystem(){
    static JPH::JobSystemSingleThreaded* instance = new JPH::JobSystemSingleThreaded(JPH::cMaxPhysicsJobs);
    return *instance;
}

JPH::ShapeRefC JoltShapeBuilder::buildConvexHullShape(const std::vector<Vector3>& points, std::string* error, float maxConvexRadius,
                                                      float hullTolerance){
    JPH::Array<JPH::Vec3> jvertices;
    jvertices.reserve(points.size());
    for (const Vector3& point : points){
        jvertices.push_back(JPH::Vec3(point.x, point.y, point.z));
    }

    JPH::ConvexHullShapeSettings shape_settings(jvertices, maxConvexRadius);
    shape_settings.mHullTolerance = hullTolerance;
    JPH::ShapeSettings::ShapeResult shape_result = shape_settings.Create();
    if (!shape_result.IsValid()){
        setError(error, "Cannot create convex hull shape: " + std::string(shape_result.GetError().c_str()));
        return NULL;
    }
    return shape_result.Get();
}

JPH::ShapeRefC JoltShapeBuilder::buildMeshShape(const std::vector<Vector3>& vertices, const std::vector<uint32_t>& indices,
                                                std::string* error, const JPH::PhysicsMaterial* material){
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
    if (material){
        // every triangle's material index is 0
        shape_settings.mMaterials.push_back(material);
    }
    JPH::ShapeSettings::ShapeResult shape_result = shape_settings.Create();
    if (!shape_result.IsValid()){
        setError(error, "Cannot create mesh shape: " + std::string(shape_result.GetError().c_str()));
        return NULL;
    }
    return shape_result.Get();
}

unsigned int JoltShapeBuilder::defaultHeightFieldSamples(const TerrainComponent& terrain){
    if (terrain.heightMap.empty() || terrain.heightMap.isFramebuffer() || !terrain.heightMap.hasData()){
        return 4;
    }
    TextureData& textureData = terrain.heightMap.getData();
    const int texels = std::min(textureData.getWidth(), textureData.getHeight());
    return (unsigned int)std::max(4, std::min(texels, (int)MAX_HEIGHTFIELD_SAMPLES));
}

void JoltShapeBuilder::sampleHeightField(Scene* scene, TerrainComponent& terrain, const Vector3& scale, unsigned int samplesSize,
                                         unsigned int x0, unsigned int y0, unsigned int w, unsigned int h, float* out){
    // Sample i of N sits at -half + size * i / (N - 1): the first and last samples are the
    // terrain's edges. Each is read through sampleTerrainSurface, the terrain vertex shader's
    // lookup (linear filtering, texel i at (i + 0.5) / W), so every sample sits on the drawn
    // surface; between samples the collider interpolates on its own grid.
    MeshSystem* meshSystem = scene->getSystem<MeshSystem>().get();
    const float halfSize = terrain.terrainSize * 0.5f;
    const float spacing = terrain.terrainSize / float(samplesSize - 1);

    // A negative scale mirrors the terrain: sample i then sits where the mirrored surface is,
    // as the collider's own axes stay positive
    const float mirrorX = scale.x < 0.0f ? -1.0f : 1.0f;
    const float mirrorZ = scale.z < 0.0f ? -1.0f : 1.0f;

    for (unsigned int y = y0; y < y0 + h; y++){
        const float localZ = mirrorZ * ((y == samplesSize - 1) ? halfSize : (-halfSize + spacing * y));
        for (unsigned int x = x0; x < x0 + w; x++){
            const float localX = mirrorX * ((x == samplesSize - 1) ? halfSize : (-halfSize + spacing * x));
            float height = 0.0f;
            Vector3 normal;
            meshSystem->sampleTerrainSurface(terrain, localX, localZ, height, normal);
            out[(x - x0) + (y - y0) * w] = height;
        }
    }
}

JPH::ShapeRefC JoltShapeBuilder::buildHeightFieldShape(Scene* scene, TerrainComponent& terrain, const Vector3& scale,
                                                       unsigned int samplesSize, std::string* error, const JPH::PhysicsMaterial* material){
    if (terrain.terrainSize <= std::numeric_limits<float>::epsilon()){
        setError(error, "Cannot create heightfield shape for a terrain without size");
        return NULL;
    }
    if (samplesSize == 0){
        samplesSize = defaultHeightFieldSamples(terrain);
    }
    samplesSize = std::max(samplesSize, 4u);

    const float halfSize = terrain.terrainSize * 0.5f;
    const float spacing = terrain.terrainSize / float(samplesSize - 1);

    std::vector<float> samples(samplesSize * samplesSize);
    sampleHeightField(scene, terrain, scale, samplesSize, 0, 0, samplesSize, samplesSize, samples.data());

    // T3 R22: the collider's gap never includes drawn ground. A terrain without a map pays this test
    if (!terrain.holeMap.empty()){
        const terrain::HoleMapView holes = terrainSurfaceView(terrain).holes;
        if (holes.texels){
            std::vector<uint8_t> open;
            std::vector<uint8_t> gaps;
            terrain::openSquares(holes, static_cast<int>(samplesSize), mirrorX, mirrorZ, open);
            terrain::noCollisionSamples(open, static_cast<int>(samplesSize), gaps);
            for (size_t i = 0; i < gaps.size(); i++){
                if (gaps[i]){
                    samples[i] = JPH::HeightFieldShapeConstants::cNoCollisionValue;
                }
            }
        }
    }

    const Vector3 absEntityScale = absScale(scale);
    const JPH::Vec3 terrainOffset(-halfSize * absEntityScale.x, 0.0f, -halfSize * absEntityScale.z);
    const JPH::Vec3 terrainScale(spacing * absEntityScale.x, absEntityScale.y, spacing * absEntityScale.z);

    JPH::HeightFieldShapeSettings shape_settings(samples.data(), terrainOffset, terrainScale, samplesSize);
    // A terrain a game may edit reserves its whole height range, so a refresh never has to clamp
    // or rebuild (T1 R77); other terrains keep the range of their samples, byte for byte
    if (terrain.allowRuntimeEdits){
        shape_settings.mMinHeightValue = std::min(0.0f, terrain.maxHeight);
        shape_settings.mMaxHeightValue = std::max(0.0f, terrain.maxHeight);
    }
    if (material){
        // one material: every square uses it (no index list)
        shape_settings.mMaterials.push_back(material);
    }
    JPH::ShapeSettings::ShapeResult shape_result = shape_settings.Create();
    if (!shape_result.IsValid()){
        setError(error, "Cannot create heightfield shape: " + std::string(shape_result.GetError().c_str()));
        return NULL;
    }
    return shape_result.Get();
}

JPH::ShapeRefC JoltShapeBuilder::buildColliderShape(Scene* scene, Entity entity, const Collider3D& shapeData, std::string* error,
                                                    const JPH::PhysicsMaterial* material){
    JPH::ShapeRefC shape = buildColliderLeaf(scene, entity, shapeData, error, material);
    // a convex leaf is the collider's only one: it carries the material itself (mesh and
    // heightfield leaves got it per triangle)
    if (shape && material && shape->GetType() == JPH::EShapeType::Convex){
        const_cast<JPH::ConvexShape*>(static_cast<const JPH::ConvexShape*>(shape.GetPtr()))->SetMaterial(material);
    }
    return shape;
}

JPH::ShapeRefC JoltShapeBuilder::buildColliderLeaf(Scene* scene, Entity entity, const Collider3D& shapeData, std::string* error,
                                                   const JPH::PhysicsMaterial* material){
    const Vector3 entityScale = entityWorldScale(scene, entity);
    const Vector3 absEntityScale = absScale(entityScale);

    if (shapeData.type == Collider3DType::BOX){
        const JPH::Vec3 halfExtent(shapeData.width * absEntityScale.x / 2.0f, shapeData.height * absEntityScale.y / 2.0f, shapeData.depth * absEntityScale.z / 2.0f);
        JPH::BoxShapeSettings shape_settings(halfExtent, fittingConvexRadius(halfExtent.ReduceMin(), true));
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
        const float halfHeight = shapeData.halfHeight * absEntityScale.y;
        const float radius = shapeData.radius * maxScaleXZ(absEntityScale);
        JPH::CylinderShapeSettings shape_settings(halfHeight, radius, fittingConvexRadius(std::min(halfHeight, radius), false));
        JPH::ShapeSettings::ShapeResult shape_result = shape_settings.Create();
        if (!shape_result.IsValid()) return NULL;
        return shape_result.Get();
    }else if (shapeData.type == Collider3DType::CONVEX_HULL){
        const Collider3DSource source = shapeData.source == Collider3DSource::NONE ? Collider3DSource::ENTITY_MESH : shapeData.source;
        std::vector<Vector3> points;

        if (source == Collider3DSource::RAW_VERTICES){
            points.resize(shapeData.vertices.size());
            for (size_t i = 0; i < shapeData.vertices.size(); i++){
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
            vertices.resize(shapeData.vertices.size());
            for (size_t i = 0; i < shapeData.vertices.size(); i++){
                vertices[i] = shapeData.vertices[i] * entityScale;
            }

            // whole triangles whose corners exist
            const size_t triangleIndices = (shapeData.indices.size() / 3) * 3;
            indices.reserve(triangleIndices);
            for (size_t i = 0; i + 2 < triangleIndices; i += 3){
                const uint32_t a = shapeData.indices[i], b = shapeData.indices[i + 1], c = shapeData.indices[i + 2];
                if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size()){
                    setError(error, "Cannot create mesh shape: triangle " + std::to_string(i / 3) + " has an index past the " +
                             std::to_string(vertices.size()) + " vertices of 3D Body entity: " + std::to_string(entity));
                    return NULL;
                }
                indices.push_back(a);
                indices.push_back(b);
                indices.push_back(c);
            }
            // mirrored by the scale: wound back so the triangles face out again
            if (entityScale.x * entityScale.y * entityScale.z < 0.0f){
                for (size_t t = 0; t + 2 < indices.size(); t += 3) std::swap(indices[t + 1], indices[t + 2]);
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

        return buildMeshShape(vertices, indices, error, material);
    }else if (shapeData.type == Collider3DType::HEIGHTFIELD){
        Entity terrainEntity = shapeData.sourceEntity == NULL_ENTITY ? entity : shapeData.sourceEntity;
        TerrainComponent* terrain = scene->findComponent<TerrainComponent>(terrainEntity);
        if (!terrain || !terrain->heightMapLoaded){
            setError(error, "Cannot create heightfield shape without loaded terrain heightmap");
            return NULL;
        }

        return buildHeightFieldShape(scene, *terrain, entityScale, shapeData.samplesSize, error, material);
    }

    return NULL;
}
#endif
