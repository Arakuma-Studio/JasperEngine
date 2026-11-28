// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TRANSFORMPROPAGATION_H
#define TRANSFORMPROPAGATION_H

#include "EntityRegistry.h"
#include "component/RectTransformComponent.h"
#include "component/Transform.h"

#include <cassert>

namespace jasperengine {

    // The propagation of the Transforms at [begin, end) of the array (parents before children,
    // the array's hierarchy order): a child whose parent needs an update needs one too and
    // inherits its visibility change; `update(transform, entity)` recomputes the matrices of
    // every Transform that needs it, and those are stamped changed (Changed<Transform>) with one
    // tick of the registry, taken at the first. `visit(index)` sees each one updated. A range
    // starts at a Transform whose parent is up to date, or the root.
    template<typename Update, typename Visit>
    void propagateTransformRange(EntityRegistry& registry, ComponentArray<Transform>& transforms, size_t begin, size_t end,
                                 Tick& tick, Update&& update, Visit&& visit){
        for (size_t i = begin; i < end; i++){
            Transform& transform = transforms.getComponentFromIndex(i);

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
                const Entity entity = transforms.getEntity(i);
                update(transform, entity);
                if (tick == 0){
                    tick = registry.getChangeClock().stamp();
                }
                transforms.markChangedAt(i, tick);
                visit(i);
            }
        }
    }

    // The transform pass of RenderSystem::update: one linear walk of the whole Transform array.
    template<typename Update>
    void propagateTransforms(EntityRegistry& registry, Update&& update){
        auto transforms = registry.getComponentArray<Transform>();
        Tick tick = 0;
        propagateTransformRange(registry, *transforms, 0, transforms->size(), tick, update, [](size_t){});
    }

}

namespace jasperengine {

    // A Transform's world pose: what its children compose with (the model matrix, world rotation
    // and scale) and its world position
    struct TransformPose {
        Vector3 position;
        Quaternion rotation;
        Vector3 scale = Vector3(1, 1, 1);
        Matrix4 modelMatrix;
    };

    // The origin of a Transform's local matrix: UI grows around its pivot, so its origin, the
    // top-left corner, moves
    inline Vector3 localMatrixOrigin(EntityRegistry& registry, const Transform& transform, Entity entity){
        Vector3 origin = transform.position;
        const RectTransformComponent* layout = registry.findComponent<RectTransformComponent>(entity);
        if (layout && layout->pivot != Vector2(0, 0)){
            origin += getUIPivotShift(*layout, transform.rotation, transform.scale);
        }
        return origin;
    }

    // One step of the composition, shared by the pass and computeWorldPose so both give the same
    // bits: every Transform matrix is affine (made here from T * R * S and products of such): the
    // affine forms compute the general products' non-zero terms in their order
    inline void composeTransformPose(const Transform& transform, const Vector3& origin, const TransformPose* parent,
                                     Matrix4& localMatrix, TransformPose& world){
        localMatrix = Matrix4::composeTRS(origin, transform.rotation, transform.scale);
        if (parent){
            assert(parent->modelMatrix.isAffine() && "a Transform's matrices are made affine here");
            world.modelMatrix = parent->modelMatrix.affineMultiply(localMatrix);
            world.position = parent->modelMatrix.transformAffine(origin);
            world.scale = parent->scale * transform.scale;
            world.rotation = parent->rotation * transform.rotation;
        }else{
            world.modelMatrix = localMatrix;
            world.position = origin;
            world.scale = transform.scale;
            world.rotation = transform.rotation;
        }
    }

    inline TransformPose transformPoseOf(const Transform& transform){
        return {transform.worldPosition, transform.worldRotation, transform.worldScale, transform.modelMatrix};
    }

    // The matrices and world values of one Transform from its local values and its parent's
    // (already up to date): what RenderSystem::updateTransform computes. The normal matrix only
    // when asked (a scene without lights skips it).
    inline void computeWorldTransform(EntityRegistry& registry, Transform& transform, Entity entity, bool normals = true){
        const Vector3 origin = localMatrixOrigin(registry, transform, entity);
        const Transform* transformParent = transform.parent != NULL_ENTITY ? registry.findComponent<Transform>(transform.parent) : nullptr;
        TransformPose parent;
        if (transformParent){
            parent = transformPoseOf(*transformParent);
        }
        TransformPose world;
        composeTransformPose(transform, origin, transformParent ? &parent : nullptr, transform.localMatrix, world);
        transform.modelMatrix = world.modelMatrix;
        transform.worldPosition = world.position;
        transform.worldScale = world.scale;
        transform.worldRotation = world.rotation;

        if (normals){
            transform.normalMatrix = transform.modelMatrix.affineInverse().transpose();
        }
    }

    // The world pose of a Transform now (S95 REQ-6), without changing anything: its stored world
    // values while neither it nor an ancestor needs an update, else composed from the local
    // values down from the last ancestor that is up to date (the bits the next pass computes).
    // One walk up the ancestors' flags when clean; d compositions for a dirty chain of depth d;
    // no allocation. The entity must have a Transform.
    inline TransformPose computeWorldPose(EntityRegistry& registry, Entity entity){
        // the chain from the entity up to the highest Transform that needs an update, the first
        // kChain of it kept here (a deeper chain composes its upper part by recursion)
        constexpr size_t kChain = 32;
        const Transform* chain[kChain];
        Entity chainEntities[kChain];
        size_t length = 0;     // entity .. highest dirty, counted
        size_t walked = 0;

        const Transform* transform = registry.findComponent<Transform>(entity);
        assert(transform && "computeWorldPose needs a Transform");
        const Transform* current = transform;
        Entity currentEntity = entity;
        while (current){
            if (walked < kChain){
                chain[walked] = current;
                chainEntities[walked] = currentEntity;
            }
            walked++;
            if (current->needUpdate){
                length = walked;
            }
            if (current->parent == NULL_ENTITY){
                break;
            }
            currentEntity = current->parent;
            current = registry.findComponent<Transform>(currentEntity);
        }
        if (length == 0){
            return transformPoseOf(*transform);
        }

        // the pose the kept part composes on: the parent of the highest dirty one (up to date, or
        // none), or for a chain longer than kept, the pose of the first one not kept
        const size_t kept = length < kChain ? length : kChain;
        TransformPose base;
        bool hasBase = false;
        if (length > kChain){
            base = computeWorldPose(registry, chain[kChain - 1]->parent);
            hasBase = true;
        }else if (const Transform* top = chain[kept - 1]; top->parent != NULL_ENTITY){
            if (const Transform* parent = registry.findComponent<Transform>(top->parent)){
                base = transformPoseOf(*parent);
                hasBase = true;
            }
        }

        TransformPose world;
        Matrix4 localMatrix;
        for (size_t i = kept; i-- > 0;){
            const Vector3 origin = localMatrixOrigin(registry, *chain[i], chainEntities[i]);
            composeTransformPose(*chain[i], origin, hasBase ? &base : nullptr, localMatrix, world);
            base = world;
            hasBase = true;
        }
        return world;
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
