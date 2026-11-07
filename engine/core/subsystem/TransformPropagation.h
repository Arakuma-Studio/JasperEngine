// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TRANSFORMPROPAGATION_H
#define TRANSFORMPROPAGATION_H

#include "EntityRegistry.h"
#include "component/RectTransformComponent.h"
#include "component/Transform.h"

#include <cassert>

namespace jasperengine {

    // The transform pass of RenderSystem::update: one linear walk of the Transform array,
    // parents before children (the array's hierarchy order). A child whose parent needs an
    // update needs one too and inherits its visibility change; `update(transform, entity)`
    // recomputes the matrices of every Transform that needs it, and those are stamped
    // changed (Changed<Transform>) with the registry's current tick.
    template<typename Update>
    void propagateTransforms(EntityRegistry& registry, Update&& update){
        auto transforms = registry.getComponentArray<Transform>();
        Tick tick = 0;
        bool stamped = false;

        for (size_t i = 0; i < transforms->size(); i++){
            Transform& transform = transforms->getComponentFromIndex(i);

            if (transform.parent != NULL_ENTITY){
                Transform* transformParent = registry.findComponent<Transform>(transform.parent);
                if (transformParent){
                    if (transformParent->needUpdate){
                        transform.needUpdate = true;
                    }

                    if (transformParent->needUpdateChildVisibility){
                        transform.visible = transformParent->visible;
                        transform.needUpdateChildVisibility = true;
                    }
                }
            }

            if (transform.needUpdate){
                const Entity entity = transforms->getEntity(i);
                update(transform, entity);
                if (!stamped){
                    tick = registry.getChangeClock().stamp();
                    stamped = true;
                }
                transforms->markChangedAt(i, tick);
            }
        }
    }

}

namespace jasperengine {

    // The matrices and world values of one Transform from its local values and its parent's
    // (already up to date): what RenderSystem::updateTransform computes. The normal matrix only
    // when asked (a scene without lights skips it).
    inline void computeWorldTransform(EntityRegistry& registry, Transform& transform, Entity entity, bool normals = true){
        // UI grows around its pivot, so its origin, the top-left corner, moves
        Vector3 origin = transform.position;
        const RectTransformComponent* layout = registry.findComponent<RectTransformComponent>(entity);
        if (layout && layout->pivot != Vector2(0, 0)){
            origin += getUIPivotShift(*layout, transform.rotation, transform.scale);
        }

        // every Transform matrix is affine (made here from T * R * S and products of such): the
        // affine forms compute the general products' non-zero terms in their order
        transform.localMatrix = Matrix4::composeTRS(origin, transform.rotation, transform.scale);

        const Transform* transformParent = transform.parent != NULL_ENTITY ? registry.findComponent<Transform>(transform.parent) : nullptr;
        if (transformParent){
            assert(transformParent->modelMatrix.isAffine() && "a Transform's matrices are made affine here");
            transform.modelMatrix = transformParent->modelMatrix.affineMultiply(transform.localMatrix);
            transform.worldPosition = transformParent->modelMatrix.transformAffine(origin);
            transform.worldScale = transformParent->worldScale * transform.scale;
            transform.worldRotation = transformParent->worldRotation * transform.rotation;
        }else{
            transform.modelMatrix = transform.localMatrix;
            transform.worldPosition = origin;
            transform.worldScale = transform.scale;
            transform.worldRotation = transform.rotation;
        }

        if (normals){
            transform.normalMatrix = transform.modelMatrix.affineInverse().transpose();
        }
    }

    // The world values of every Transform that needs an update, parents first, without the
    // rest of the render pass: needUpdate stays set, so the next RenderSystem::update still
    // does its whole work for them (bounds, cameras, lights). For what reads world values
    // before a frame ran: a scene just loaded (files hold no world values), a Prefab's
    // registry, a Prefab just instantiated.
    inline void updateWorldTransforms(EntityRegistry& registry){
        propagateTransforms(registry, [&registry](Transform& transform, Entity entity){
            computeWorldTransform(registry, transform, entity);
        });
    }

}

#endif // TRANSFORMPROPAGATION_H
