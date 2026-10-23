// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SHADOWSLOTPLANNER_H
#define SHADOWSLOTPLANNER_H

#include "Export.h"
#include "render/shadow/ShadowCache.h"

#include <cstdint>
#include <vector>

namespace jasperengine {

    enum class ShadowLightKind : int {
        DIRECTIONAL,    // cascades in the projective atlas
        SPOT,           // one slot of the projective atlas
        POINT           // six cube faces in the point atlas
    };

    // A shadow-casting light asking for slots this frame
    struct ShadowLightRequest {
        uint32_t id = 0;                // stable identity (the light's entity)
        ShadowLightKind kind = ShadowLightKind::SPOT;
        float importance = 0.0f;        // directional: intensity; local: intensity * screen coverage / (1 + distance)
        unsigned int resolution = 1024; // texels per side of each of its maps (rounded down to a power of two)
        unsigned int slots = 1;         // directional: cascades; spot: 1; point: 6
        bool wantsStatic = false;       // cached, and reached by static and dynamic casters: a static layer pays
    };

    static constexpr unsigned int SHADOW_LIGHT_MAX_SLOTS = 6;

    struct ShadowLightAssignment {
        int firstSlot = -1;             // projective (directional, spot) or point slot index; -1: no slot this frame
        // what each of its slots got (the memory cap may lower it: a directional light's
        // farthest cascades first)
        unsigned int resolution[SHADOW_LIGHT_MAX_SLOTS] = {};
        bool staticLayer = false;       // its slots have rects in the static cache
    };

    // Where every slot of one atlas is
    struct ShadowAtlasLayout {
        unsigned int width = 0;
        unsigned int height = 0;
        std::vector<ShadowRect> rects;  // by slot index (empty rect: unused)

        uint64_t bytes() const { return (uint64_t)width * (uint64_t)height * 4ull; }
        bool operator==(const ShadowAtlasLayout& o) const;
        bool operator!=(const ShadowAtlasLayout& o) const { return !(*this == o); }
    };

    struct ShadowPlan {
        std::vector<ShadowLightAssignment> lights;   // in the order of the requests
        ShadowAtlasLayout projective;
        ShadowAtlasLayout point;
        ShadowAtlasLayout projectiveStatic;          // rects of the static layers of projective slots
        ShadowAtlasLayout pointStatic;
        uint64_t memoryBytes = 0;                    // the four atlases
        bool reducedResolution = false;              // the memory cap lowered a resolution
    };

    // Gives shadow slots to lights by importance and packs them into atlases (spec S10 R15): a
    // light keeps its slots (sticky) until a challenger beats it by a margin after a cooldown,
    // every map keeps its own resolution (power-of-two squares packed without overlap), and the
    // atlases plus static caches stay within a memory budget: static layers are dropped, then
    // the least important maps halved, before anything goes over. Pure CPU, no sokol.
    class JASPERENGINE_API ShadowSlotPlanner {
    public:
        struct Limits {
            int projectiveSlots = 9;            // MAX_SHADOW_ATLAS_SLOTS
            int pointSlots = 24;                // MAX_POINT_SHADOW_ATLAS_SLOTS
            unsigned int maxTextureSize = 16384;
            unsigned int maxResolution = 4096;  // the tier's slot resolution
            unsigned int minResolution = 256;
            uint64_t memoryBytes = 0;           // 0: no budget
            float margin = 1.25f;               // a challenger needs this much more importance
            uint64_t cooldownFrames = 30;       // and the holder this many frames with its slots
        };

        const ShadowPlan& plan(const std::vector<ShadowLightRequest>& requests, const Limits& limits, uint64_t frame);
        const ShadowPlan& getPlan() const { return current; }
        void reset();

        // packs power-of-two squares (texel sizes, by slot index; 0 = unused) into the smallest
        // atlas no side of which exceeds maxSide; false when they cannot fit
        static bool pack(const std::vector<unsigned int>& sizes, unsigned int maxSide, ShadowAtlasLayout& layout);
        // the largest power of two <= resolution, clamped to [minimum, maximum]
        static unsigned int powerOfTwoResolution(unsigned int resolution, unsigned int minimum, unsigned int maximum);

    private:
        struct Holder {
            uint32_t id = 0;
            ShadowLightKind kind = ShadowLightKind::SPOT;
            int firstSlot = -1;
            unsigned int slots = 0;
            uint64_t since = 0;
        };

        std::vector<Holder> holders;
        ShadowPlan current;
    };

}

#endif // SHADOWSLOTPLANNER_H
