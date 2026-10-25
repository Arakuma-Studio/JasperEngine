// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TRANSFORMPROPAGATION_H
#define TRANSFORMPROPAGATION_H

#include "EntityRegistry.h"
#include "component/Transform.h"

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

#endif // TRANSFORMPROPAGATION_H
