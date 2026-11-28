// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef AudioSourceSlots_h
#define AudioSourceSlots_h

#include "Export.h"
#include "Entity.h"

#include <cstddef>
#include <vector>

namespace jasperengine {

    class Scene;
    class EntityHandle;
    class AudioSource;

    // Several Audio Sources on one GameObject (docs/specs/S97-audio-gaps.md REQ-24, D6): the first
    // is the GameObject's own AudioSourceComponent; each further one is a slot, a child with a
    // Transform at the local origin and an AudioSourceComponent whose `slot` is 1, 2, ... It is an
    // ordinary child entity everywhere (copies, prefabs, persistence, an engine without slots);
    // the editor's listings and the scripts' child APIs and GameObject.find skip it, and the
    // Inspector shows it as another Audio Source.
    class JASPERENGINE_API AudioSourceSlots {
    public:
        // a slot: an AudioSourceComponent with slot > 0 on a child of a GameObject
        static bool isSlot(Scene* scene, Entity entity);
        // the GameObject a slot belongs to (NULL_ENTITY when `entity` is not a slot)
        static Entity ownerOf(Scene* scene, Entity entity);
        // the slots of `owner`, in slot order (then entity order)
        static std::vector<Entity> getSlots(Scene* scene, Entity owner);
        // one past the highest slot of `owner`
        static int nextSlot(Scene* scene, Entity owner);

        // the GameObject's own source first (when it has one), then its slots in slot order
        static std::vector<AudioSource> getAll(const EntityHandle& gameObject);
        // the GameObject's own source when it has none, else a new slot (Spatial Blend 1, as Add
        // Component); a GameObject without a Transform gets one (its own source stays 2D)
        static AudioSource add(const EntityHandle& gameObject);
        // a slot goes with its entity; the GameObject's own source is removed from it (its slots stay)
        static void remove(const AudioSource& source);

        // The load-time integrity pass (REQ-29): a slot that is not under a GameObject becomes an
        // ordinary GameObject (its slot cleared, a warning naming it); the slots of one GameObject
        // that share a number are numbered again, in slot then entity order. The repairs made.
        static size_t repair(Scene* scene);
    };

}

#endif /* AudioSourceSlots_h */
