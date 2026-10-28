// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SurfacePlacement.h"

#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"
#include "math/Matrix4.h"

#include <algorithm>
#include <cfloat>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    Matrix4 localMatrix(const Transform& transform){
        return Matrix4::translateMatrix(transform.position) * transform.rotation.getRotationMatrix() * Matrix4::scaleMatrix(transform.scale);
    }

}

AABB sceneview::familyLocalBounds(Scene* scene, Entity root){
    AABB bounds;
    if (!scene || !scene->isEntityCreated(root)){
        return bounds;
    }
    auto addBounds = [&](Entity entity, const Matrix4& toRoot){
        if (const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
            const AABB& local = mesh->aabb;
            if (!local.isNull() && !local.isInfinite()){
                bounds.merge(toRoot * local);
            }
        }
    };
    addBounds(root, Matrix4());
    auto transforms = scene->getComponentArray<Transform>();
    for (size_t i = 0; i < transforms->size(); i++){
        const Entity entity = transforms->getEntity(i);
        if (entity == root){
            continue;
        }
        // the chain of local matrices up to root (none when root is not an ancestor)
        Matrix4 toRoot;
        bool below = false;
        Entity current = entity;
        for (size_t depth = 0; current != NULL_ENTITY && depth < 4096; depth++){
            const Transform* transform = scene->findComponent<Transform>(current);
            if (!transform){
                break;
            }
            if (transform->parent == root){
                toRoot = localMatrix(*transform) * toRoot;
                below = true;
                break;
            }
            toRoot = localMatrix(*transform) * toRoot;
            current = transform->parent;
        }
        if (below){
            addBounds(entity, toRoot);
        }
    }
    return bounds;
}

Quaternion sceneview::rotationBetween(const Vector3& a, const Vector3& b){
    const float d = a.dotProduct(b);
    if (d >= 1.0f - 1e-7f){
        return Quaternion();
    }
    if (d <= -1.0f + 1e-7f){
        Vector3 axis = a.perpendicular();
        axis.normalize();
        return Quaternion(0.0f, axis.x, axis.y, axis.z);
    }
    const Vector3 c = a.crossProduct(b);
    Quaternion q(1.0f + d, c.x, c.y, c.z);
    q.normalize();
    return q;
}

void sceneview::restOnSurface(const Vector3& point, const Vector3& surfaceNormal, const AABB& localBounds, const Vector3& scale,
                              const Quaternion& rotation, bool align, Vector3& position, Quaternion& outRotation){
    Vector3 normal = surfaceNormal;
    if (normal.squaredLength() < 1e-12f){
        normal = Vector3(0, 1, 0);
    }
    normal.normalize();
    outRotation = rotation;
    if (align){
        // the object's own up axis turned onto the normal, its heading kept
        Vector3 up = rotation * Vector3(0, 1, 0);
        up.normalize();
        outRotation = rotationBetween(up, normal) * rotation;
        outRotation.normalize();
    }
    position = point;
    if (localBounds.isNull() || localBounds.isInfinite()){
        return;
    }
    // the corner of the bounds deepest along -normal touches the surface
    const Vector3 mn = localBounds.getMinimum();
    const Vector3 mx = localBounds.getMaximum();
    float deepest = FLT_MAX;
    for (int corner = 0; corner < 8; corner++){
        const Vector3 local((corner & 1) ? mx.x : mn.x, (corner & 2) ? mx.y : mn.y, (corner & 4) ? mx.z : mn.z);
        const Vector3 offset = outRotation * Vector3(local.x * scale.x, local.y * scale.y, local.z * scale.z);
        deepest = std::min(deepest, offset.dotProduct(normal));
    }
    position = point - normal * deepest;
}
