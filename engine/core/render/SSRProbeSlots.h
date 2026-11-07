// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SSRPROBESLOTS_H
#define SSRPROBESLOTS_H

#include "Export.h"
#include "ecs/Entity.h"
#include "math/AABB.h"
#include "math/Matrix4.h"

#include <cstdint>
#include <vector>

namespace jasperengine{

    // The reflection the SSR composite swaps per pixel (docs/rendering/reflections.md). The G-buffer
    // pass writes an 8-bit code in color[2].a; composite.frag and ssr.frag read it through
    // includes/ssr_source.glsl, whose constants must match these.

    // the local reflection probes the composite can rebuild in one view (u_probeTexture0..3)
    constexpr int SSR_PROBE_SLOTS = 4;
    // the probe blend weight's steps in the code
    constexpr int SSR_PROBE_WEIGHT_STEPS = 62;

    // what a submesh's colour pass adds as its environment specular, from its shader variant
    enum class SSRSource : uint8_t{
        SKIP,        // nothing SSR may replace or add to: unlit materials, planar mirrors
        ADD,         // an environment the composite cannot rebuild, or none: SSR is added unoccluded
        AMBIENT,     // the built-in Lit without IBL: the scene ambient's specular
        ENVIRONMENT  // the built-in Lit with IBL: the sky, or the local probe over it (per draw)
    };

    namespace SSRCode{
        constexpr uint8_t SKIP = 0;
        constexpr uint8_t ADD = 1;
        constexpr uint8_t AMBIENT = 2;
        constexpr uint8_t SKY = 3;
        constexpr uint8_t PROBE_NO_SLOT = 4;
        constexpr uint8_t PROBE_BASE = 5;

        // a probe in `slot` blended over the sky by `weight` (0..1, at least one step)
        JASPERENGINE_API uint8_t probe(int slot, float weight);
        // the slot and weight step (1..SSR_PROBE_WEIGHT_STEPS) of a probe code; false for any other code
        JASPERENGINE_API bool decodeProbe(uint8_t code, int& slot, int& step);
    }

    static_assert(SSRCode::PROBE_BASE + SSR_PROBE_SLOTS * SSR_PROBE_WEIGHT_STEPS <= 256,
                  "the SSR reflection codes must fit one byte");

    // the fraction (0..1) of the screen a world box's projection covers, the box clipped to the
    // space in front of the eye (a box around the eye covers all of it)
    JASPERENGINE_API float screenCoverage(const AABB& box, const Matrix4& viewProjection);

    struct SSRProbeCandidate{
        Entity probe = NULL_ENTITY;
        float coverage = 0.0f; // the summed screen coverage of the drawn meshes it lights
    };

    struct SSRProbeAssignment{
        Entity slots[SSR_PROBE_SLOTS] = {NULL_ENTITY, NULL_ENTITY, NULL_ENTITY, NULL_ENTITY};

        // -1 when the probe has no slot
        int slotOf(Entity probe) const;
        int count() const;
    };

    // A probe that held a slot last frame competes with this much more coverage, so two probes of
    // about the same size do not trade slots (and SSR) every frame.
    constexpr float SSR_PROBE_HYSTERESIS = 1.25f;

    // Gives the slots to the candidates covering the most screen (ties: the lower entity). A probe
    // keeps last frame's slot index when it keeps a slot, so the codes of what does not change stay.
    JASPERENGINE_API void assignSSRProbeSlots(const std::vector<SSRProbeCandidate>& candidates,
                                              const SSRProbeAssignment& previous, SSRProbeAssignment& out);

}

#endif //SSRPROBESLOTS_H
