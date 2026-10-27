// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The fixed ring pool, the spacing rule and fade-after-death follow OpenEngine's
// ParticleTrailStore (Engine/Modules/Particles/Source/ParticleTrailStore.cpp), Copyright (c)
// 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#ifndef PARTICLES_PARTICLETRAILS_H
#define PARTICLES_PARTICLETRAILS_H

// Per-particle trails (docs/specs/S20-particles.md R9): a pool of rings sized at bind, one ring
// per trailed particle (plus as many again for the trails of dead particles fading out). Ring
// points are SoA: x, y, z and birth time, 16 bytes a point. The head point follows its
// particle until the particle is minVertexDistance from the point before it, then a new head
// starts; points older than the trail lifetime drop off the tail.

#include "Export.h"

#include <cstdint>
#include <vector>

namespace jasperengine { namespace particles {

    inline constexpr uint32_t kNoTrail = 0xffffffffu;

    class JASPERENGINE_API ParticleTrails {
    public:
        // rings of `points` points; `rings` of them
        void configure(uint32_t rings, uint32_t points);
        void clear();

        // a free ring for a particle (kNoTrail when the pool is out)
        uint32_t acquire();
        // the particle died: the ring is freed now, or kept to fade (its points expire)
        void release(uint32_t ring, bool fade);

        // records the particle's position into its ring (the spacing rule)
        void record(uint32_t ring, float x, float y, float z, float time, float minDistance);
        // drops the points older than their ring's lifetime and frees the orphaned rings left empty
        void expire(float time);
        void setLifetime(uint32_t ring, float lifetime);
        // what the trail inherits of its particle at birth: the start colour (RGBA8 sRGB) and size
        void setStyle(uint32_t ring, uint32_t color, float width);
        uint32_t color(uint32_t ring) const { return colors[ring]; }
        float width(uint32_t ring) const { return widths[ring]; }

        uint32_t ringCount() const { return rings; }
        uint32_t pointsPerRing() const { return points; }
        uint32_t live(uint32_t ring) const { return counts[ring]; }
        bool active(uint32_t ring) const { return state[ring] != Free; }
        bool orphaned(uint32_t ring) const { return state[ring] == Orphan; }
        float lifetime(uint32_t ring) const { return lifetimes[ring]; }
        // point `k` of a ring: 0 the newest
        void point(uint32_t ring, uint32_t k, float& x, float& y, float& z, float& time) const;
        uint32_t usedRings() const { return used; }
        size_t bytesPerPoint() const { return 4 * sizeof(float); }
        size_t memoryBytes() const { return xs.size() * 4 * sizeof(float) + (size_t)rings * (3 * sizeof(uint32_t) + 2 * sizeof(float) + 1); }

    private:
        enum State : uint8_t { Free, Owned, Orphan };
        uint32_t rings = 0;
        uint32_t points = 0;
        uint32_t used = 0;
        std::vector<float> xs, ys, zs, times;   // rings * points
        std::vector<uint32_t> heads;            // the newest point's slot
        std::vector<uint32_t> counts;
        std::vector<uint8_t> state;
        std::vector<float> lifetimes;
        std::vector<uint32_t> colors;
        std::vector<float> widths;
        std::vector<uint32_t> freeList;
    };

}}

#endif // PARTICLES_PARTICLETRAILS_H
