// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UIWorldRaycast.h"
#include "model/ModelCpuData.h"

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"

#include <cmath>
#include <limits>
#include <string>

using namespace jasperengine;

namespace {

    // one attribute and the buffer it reads
    struct AttributeView {
        Buffer* buffer = nullptr;
        Attribute attribute;

        bool valid() const{ return buffer != nullptr; }
    };

    // the buffers of a mesh by the names attributes use ("vertices", "indices", external names;
    // the names MeshSystem gives submesh attributes)
    Buffer* findBuffer(MeshComponent& mesh, const std::string& name){
        ModelCpuData::ensure(mesh);
        if (name == "vertices")
            return &mesh.buffer;
        if (name == "indices")
            return &mesh.indices;
        for (unsigned int i = 0; i < mesh.numExternalBuffers; i++){
            if (name == mesh.eBuffers[i].getName())
                return &mesh.eBuffers[i];
        }
        return nullptr;
    }

    // the component-level attribute: the interleaved buffer first, then every external buffer
    AttributeView findComponentAttribute(MeshComponent& mesh, AttributeType type){
        ModelCpuData::ensure(mesh);
        AttributeView view;
        auto take = [&view, type](Buffer& buffer){
            if (view.valid() || buffer.getSize() == 0)
                return;
            if (Attribute* attribute = buffer.getAttribute(type)){
                view.buffer = &buffer;
                view.attribute = *attribute;
            }
        };
        if (type == AttributeType::INDEX){
            take(mesh.indices);
        }else{
            take(mesh.buffer);
        }
        for (unsigned int i = 0; i < mesh.numExternalBuffers; i++){
            take(mesh.eBuffers[i]);
        }
        return view;
    }

    // a submesh's own attribute overrides the component's (an empty buffer name keeps the
    // component buffer; an unknown one drops it, its offset meaning nothing elsewhere)
    AttributeView submeshAttribute(MeshComponent& mesh, const Submesh& submesh, AttributeType type, const AttributeView& component){
        auto it = submesh.attributes.find(type);
        if (it == submesh.attributes.end())
            return component;

        AttributeView view;
        view.buffer = it->second.getBufferName().empty() ? component.buffer : findBuffer(mesh, it->second.getBufferName());
        view.attribute = it->second;
        return view;
    }

    unsigned int elementCount(const AttributeView& view){
        const unsigned int count = view.attribute.getCount();
        return count ? count : view.buffer->getCount();
    }

    // two-sided Möller–Trumbore: t along the ray, (u, v) barycentric of v1 and v2
    bool intersectTriangle(const Vector3& origin, const Vector3& direction, const Vector3& v0, const Vector3& v1, const Vector3& v2,
                           float& t, float& u, float& v){
        const Vector3 edge1 = v1 - v0;
        const Vector3 edge2 = v2 - v0;
        const Vector3 p = direction.crossProduct(edge2);
        const float det = edge1.dotProduct(p);
        if (std::fabs(det) < 1e-12f)
            return false;   // parallel or degenerate

        const float invDet = 1.0f / det;
        const Vector3 s = origin - v0;
        u = s.dotProduct(p) * invDet;
        if (u < 0.0f || u > 1.0f)
            return false;

        const Vector3 q = s.crossProduct(edge1);
        v = direction.dotProduct(q) * invDet;
        if (v < 0.0f || u + v > 1.0f)
            return false;

        t = edge2.dotProduct(q) * invDet;
        return true;
    }

}

UIWorldHit jasperengine::raycastMeshUV(Scene* scene, Entity meshEntity, const Ray& worldRay){
    UIWorldHit result;
    if (!scene || meshEntity == NULL_ENTITY)
        return result;

    MeshComponent* mesh = scene->findComponent<MeshComponent>(meshEntity);
    Transform* transform = scene->findComponent<Transform>(meshEntity);
    if (!mesh || !transform || !mesh->enabled || !transform->visible || mesh->numSubmeshes == 0)
        return result;

    const Vector3 worldDirection = worldRay.getDirection();
    const float rayLength = worldDirection.length();
    if (!(rayLength > 0.0f))
        return result;

    // the ray in mesh space: the hit parameter t is the same in both spaces
    const Matrix4 inverseModel = transform->modelMatrix.inverse();
    const Vector3 origin = inverseModel * worldRay.getOrigin();
    const Vector3 direction = (inverseModel * (worldRay.getOrigin() + worldDirection)) - origin;

    const AttributeView componentPositions = findComponentAttribute(*mesh, AttributeType::POSITION);
    const AttributeView componentUVs = findComponentAttribute(*mesh, AttributeType::TEXCOORD1);
    const AttributeView componentIndices = findComponentAttribute(*mesh, AttributeType::INDEX);

    float bestT = std::numeric_limits<float>::max();

    for (unsigned int s = 0; s < mesh->numSubmeshes; s++){
        const Submesh& submesh = mesh->submeshes[s];
        if (submesh.primitiveType != PrimitiveType::TRIANGLES && submesh.primitiveType != PrimitiveType::TRIANGLE_STRIP)
            continue;

        const AttributeView positions = submeshAttribute(*mesh, submesh, AttributeType::POSITION, componentPositions);
        const AttributeView uvs = submeshAttribute(*mesh, submesh, AttributeType::TEXCOORD1, componentUVs);
        const AttributeView indices = submeshAttribute(*mesh, submesh, AttributeType::INDEX, componentIndices);
        if (!positions.valid() || !uvs.valid())
            continue;

        const unsigned int vertexCount = std::min(positions.buffer->getCount(), uvs.buffer->getCount());
        const bool indexed = indices.valid() && indices.buffer->getSize() > 0;
        const unsigned int count = indexed ? elementCount(indices) : elementCount(positions);
        const bool wideIndices = indexed && indices.attribute.getDataType() == AttributeDataType::UNSIGNED_INT;

        Attribute positionAttribute = positions.attribute;
        Attribute uvAttribute = uvs.attribute;
        Attribute indexAttribute = indices.attribute;
        auto vertexAt = [&](unsigned int i) -> unsigned int {
            if (!indexed)
                return i;
            return wideIndices ? indices.buffer->getUInt32(&indexAttribute, i) : indices.buffer->getUInt16(&indexAttribute, i);
        };

        const bool strip = submesh.primitiveType == PrimitiveType::TRIANGLE_STRIP;
        const unsigned int triangles = strip ? (count >= 3 ? count - 2 : 0) : count / 3;
        for (unsigned int tri = 0; tri < triangles; tri++){
            const unsigned int first = strip ? tri : tri * 3;
            const unsigned int i0 = vertexAt(first), i1 = vertexAt(first + 1), i2 = vertexAt(first + 2);
            if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
                continue;

            const Vector3 v0 = positions.buffer->getVector3(&positionAttribute, i0);
            const Vector3 v1 = positions.buffer->getVector3(&positionAttribute, i1);
            const Vector3 v2 = positions.buffer->getVector3(&positionAttribute, i2);

            float t, u, v;
            if (!intersectTriangle(origin, direction, v0, v1, v2, t, u, v))
                continue;
            // in front of the origin and within the ray's length
            if (t < 0.0f || t > 1.0f || t >= bestT)
                continue;

            const Vector2 uv0 = uvs.buffer->getVector2(&uvAttribute, i0);
            const Vector2 uv1 = uvs.buffer->getVector2(&uvAttribute, i1);
            const Vector2 uv2 = uvs.buffer->getVector2(&uvAttribute, i2);
            Vector2 uv = uv0 * (1.0f - u - v) + uv1 * u + uv2 * v;
            // what the mesh shader samples (sprites and atlas tiles)
            if (submesh.hasTextureRect){
                const Rect& rect = submesh.textureRect;
                uv = Vector2(rect.getX() + uv.x * rect.getWidth(), rect.getY() + uv.y * rect.getHeight());
            }

            bestT = t;
            result.hit = true;
            result.uv = uv;
            result.distance = t * rayLength;
        }
    }

    return result;
}
