// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R13, §6.2)
// - Purpose: the triangle soup the mesher's solids write before TitleMeshBuilder welds and
//   optimises it (Pachislop's MeshBuilder): triangles per material slot, each vertex with its
//   layer-space position and normal, the current span-table entry and part (a solid's z range and
//   rig) and the slot's role, plus the arc length along its source outline for the wrap UV.
// - Thread-safety: one RawMesh per job; merged on the job's owner in a fixed order.

#pragma once

#include <cstdint>
#include <vector>

namespace jasperengine::titleart::three_d {

    // what a slot draws (the shader's image projection and part renders read it)
    enum class SlotRole : uint8_t { Face, Bevel, Side, BandFront, BandSide };

    struct RawVertex {
        float x, y, z;
        float nx, ny, nz;
        uint16_t span;
        uint16_t part;
        float arc;          // arc length along the source outline (layer units); caps: unused
    };

    struct RawMesh {
        std::vector<std::vector<RawVertex>> slots;  // per material slot: 3 vertices per triangle
        uint16_t span = 0;
        uint16_t part = 0;

        void push(int slot, double x, double y, double z, double nx, double ny, double nz, double arc = 0.0) {
            if ((size_t)slot >= slots.size()) slots.resize((size_t)slot + 1);
            slots[(size_t)slot].push_back(
                {(float)x, (float)y, (float)z, (float)nx, (float)ny, (float)nz, span, part, (float)arc});
        }
        size_t triangleCount() const {
            size_t n = 0;
            for (const auto& s : slots) n += s.size() / 3;
            return n;
        }
        // appends `other`'s triangles slot by slot (job results merged in order)
        void append(const RawMesh& other) {
            if (other.slots.size() > slots.size()) slots.resize(other.slots.size());
            for (size_t i = 0; i < other.slots.size(); i++) {
                slots[i].insert(slots[i].end(), other.slots[i].begin(), other.slots[i].end());
            }
        }
    };

}
