// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ENTITYCLONER_H
#define ENTITYCLONER_H

#include "Export.h"
#include "Entity.h"
#include <unordered_map>
#include <vector>

namespace jasperengine {

    class EntityRegistry;
    class Scene;

    // Copies GameObjects with their components, the way Unity's Instantiate copies an
    // object: what the components hold is copied, what the engine made of it at runtime
    // (GPU buffers, physics bodies, sound voices, script instances, event subscriptions)
    // is not, so the copy loads on its own like a GameObject just made. Entity references
    // to a copied GameObject are changed to its copy; references to other GameObjects are
    // kept when copying within one registry and cleared across registries.
    class JASPERENGINE_API EntityCloner {
    public:
        // `root`, its Transform descendants (in hierarchy order), and the GameObjects
        // without a Transform that belong to them: actions targeting one of them (and their
        // own actions), a model's animations and an animation's tracks. Foliage a terrain
        // streams in is left out.
        static std::vector<Entity> collectHierarchy(EntityRegistry* registry, Entity root);

        // Copies `sources` (collectHierarchy order: parents before children) from `source`
        // into `target`. The copy of sources[0] is `targetRoot` when given (a bare entity:
        // it gets the components), else a new entity; it is left at the top of the hierarchy.
        // Returns the copies in the same order; `map` (optional) gets source -> copy.
        static std::vector<Entity> clone(EntityRegistry* source, const std::vector<Entity>& sources, Scene* target,
                                         Entity targetRoot = NULL_ENTITY, std::unordered_map<Entity, Entity>* map = nullptr);

        // The move of `sources` (collectHierarchy order) from `source` to `target`, keeping what
        // they run (persistence): copies like clone(), but the copies take over the originals'
        // runtime state, which the originals no longer hold, so destroying them afterwards (the
        // caller does: ObjectManager::moveToScene) stops, releases and calls nothing. Taken over:
        // script instances and their bound properties, sound voices, running actions, tweens,
        // particles, lattice jiggle, the subscriptions of component events, body velocities (the
        // target's physics makes the bodies), Animation Players and Pose Animators when every id
        // is kept. Reloaded as for a clone: GPU state, render targets, UI documents, fluids,
        // Vertex Animators. A joint to a body outside `sources` is dropped with a warning.
        // Each GameObject keeps its id when it is free in `target` (user ids), else gets a new
        // one; `map` gets source -> copy. Returns the copies in the same order.
        static std::vector<Entity> move(Scene* source, const std::vector<Entity>& sources, Scene* target,
                                        std::unordered_map<Entity, Entity>& map);
    };

}

#endif // ENTITYCLONER_H
