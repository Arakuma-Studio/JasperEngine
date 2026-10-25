// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SHADOWCASTERREGISTRY_H
#define SHADOWCASTERREGISTRY_H

#include "Export.h"
#include "render/shadow/ShadowTypes.h"

#include <cstdint>
#include <vector>

namespace jasperengine {

    // What one shadow caster puts into shadow maps, sampled by RenderSystem once per update
    struct ShadowCasterState {
        ShadowBounds bounds;            // world bounds (all instances of a batch)
        uint32_t revision = 0;          // bumped on every change bounds alone may miss (MeshComponent::shadowCasterRevision)
        uint32_t layerBit = 0;          // 1 << the entity's layer
        bool isStatic = false;          // MeshComponent::staticShadowCaster
        bool cameraDependent = false;   // drawn per camera (terrain cut, turned billboards, culled instances)
        bool animated = false;          // skinned or morphing and animated this update: changed every update
    };

    // A caster that changed since the previous update, removed or added included: the slots its
    // old or new bounds touch are dirty
    struct ShadowCasterChange {
        ShadowBounds oldBounds;         // invalid: it was not a caster
        ShadowBounds newBounds;         // invalid: it is not a caster any more
        uint32_t layerBits = 0;         // its old and new layer
        bool staticLayer = false;       // the static layer is touched (it was or is static)
        bool dynamicLayer = false;      // the dynamic layer is touched (it was or is dynamic)
        bool cameraDependent = false;
    };

    // The shadow casters of a scene and what changed in them since the previous update: the
    // input of the shadow cache (ShadowCache). Casters are keyed by entity; a caster not updated
    // between beginUpdate and endUpdate is gone (component removed, entity destroyed or
    // inactive, castShadows off) and reported as removed. Pure CPU, allocation-free once warm.
    class JASPERENGINE_API ShadowCasterRegistry {
    public:
        void beginUpdate();
        // a caster as it is this update; returns true when it changed
        bool update(uint32_t entity, const ShadowCasterState& state);
        void endUpdate();
        // forgets every caster, and reports nothing (a scene change: every slot is redrawn anyway)
        void clear();

        // the changes of the last update (beginUpdate..endUpdate)
        const std::vector<ShadowCasterChange>& getChanges() const { return changes; }
        // the union of every caster's bounds, and of the static and dynamic ones
        const ShadowBounds& getBounds() const { return bounds; }
        const ShadowBounds& getStaticBounds() const { return staticBounds; }
        const ShadowBounds& getDynamicBounds() const { return dynamicBounds; }
        uint32_t getStaticLayers() const { return staticLayers; }
        uint32_t getDynamicLayers() const { return dynamicLayers; }
        uint32_t getCameraDependentLayers() const { return cameraLayers; }
        size_t size() const { return liveCount; }
        uint64_t getUpdate() const { return updateCount; }

    private:
        struct Record {
            ShadowCasterState state;
            uint64_t seen = 0;      // the update it was last reported in
            bool live = false;
        };

        static bool same(const ShadowCasterState& a, const ShadowCasterState& b);
        void addChange(const ShadowCasterState* oldState, const ShadowCasterState* newState);

        std::vector<Record> records;            // by entity
        std::vector<uint32_t> liveEntities;     // the entities with a live record
        std::vector<ShadowCasterChange> changes;
        ShadowBounds bounds;
        ShadowBounds staticBounds;
        ShadowBounds dynamicBounds;
        uint32_t staticLayers = 0;
        uint32_t dynamicLayers = 0;
        uint32_t cameraLayers = 0;
        size_t liveCount = 0;
        uint64_t updateCount = 0;
    };

}

#endif // SHADOWCASTERREGISTRY_H
